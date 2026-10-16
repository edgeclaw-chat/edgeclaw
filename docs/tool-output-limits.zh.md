# 工具输出的上限：一个累加器，三层各有说法

> 状态：设计稿（未实现）
> 目标读者：改 `src/toolbox` 的人
> 触发：`!cmd` 去掉用户命令的超时之后，`!yes` 这类"永不退出又狂输出"的命令没有兜底了

## 1. 现状（已逐条核实）

**全仓只有一个输出上限**，在 `edc_toolbox_actor.c:152-156`：

```c
req->result = result;
if (sdslen(result) > conf->tool_max_output_size) {
    sdsrange(result, 0, conf->tool_max_output_size);     /* 就地改这个 sds */
    result = sdscatprintf(result, "\n\n[Result truncated to 10000 characters, total length: %zu]", len);
}
```

它坐在**工具结果回给调度器的那个出口**上，所以它决定的是：

| 去向 | 有上限吗 | 原因 |
|---|---|---|
| turn 里的 `tool` 消息 → **模型** | ✅ | 就是这个 sds，被就地裁了 |
| tool-only 任务的 `task->output`（→ `$VAR` / `|` 注入的内容） | ✅ | 同一个 sds 传下去 |
| **内存里捕获**（`sdscatlen`） | ❌ | 数据进来时就已经全在内存 |
| **TUI 工具气泡**（TOOL_EMIT） | ❌ | `emit` 先 `sdsnew(content)` 拷一份，之后那行 `sdsrange` 改的是原 sds，动不到副本 |
| **chat.jsonl** | ❌ | TUI 原样写 `entry.content`（`edc_chat_transcript.cpp:76`） |

三个要点：

1. **它是事后裁剪**。截断发生时完整输出早已全在内存里；`shell_ctx_complete` 拼 `result` 时还会再复制一份（峰值 ≈ 2×）。所以 `tool_max_output_size` 对 OOM **毫无帮助**。
2. **界面的渲染已经是有界的**：`ToolMessage` 折叠时只画前 `kPreviewLines` 行（`edc_message.cpp` 的 `FirstLines`）。所以"界面这条路"的真实成本是**持有 + 落盘**，不是画。
   → 推论：**只要在捕获层就限住，内存、气泡、chat.jsonl 会一起变得有界**。
3. 附带缺陷：截断文案里的 **10000 是写死的**，实际裁剪用配置值——配置改成 30000，提示就说谎。

**同一类无上限的地方还有**（都属于"把外部数据无限攒进内存"）：

- `device_shell` / `device_file` 的 `resp_buf`（收包累加）、`device_file` 下载把整个 body 攒在内存里；
- `read_file` / `edit_file` 把整个文件读进内存（读一个 10 GB 的文件同样 OOM）。

## 2. 目标

1. **内存不随输出量增长**（在源头限住，而不是事后裁）。
2. **不误杀合法输出**：verbose 的 `!make -j8` 只被截断，不被杀；真正跑飞的能自己收敛。
3. **截断如实说明**：保留了多少 / 一共多少 / 输出是丢了还是留着。
4. **一处实现，所有累加型工具共用**（shell / device / fs 不再各写各的）。
5. **不改"给模型那一份"的语义**（`tool_max_output_size` 保留，只修文案）。

## 3. 方案

### 3.1 一个可设上限的累加器（新模块 `src/toolbox/edc_output_buf.{c,h}`）

```c
struct edc_output_buf
{
    sds data;        /* 最多 cap 字节 */
    size_t total;    /* 一共收到多少（不受 cap 影响） */
    size_t cap;
    bool truncated;
};

void edc_output_buf_init(struct edc_output_buf *b, size_t cap);
void edc_output_buf_append(struct edc_output_buf *b, const char *p, size_t n); /* 够 cap 就只计数 */
bool edc_output_buf_over(const struct edc_output_buf *b, size_t hard_max);     /* total > hard_max ? */
/* 把"kept the first X of Y"格式化到调用者的缓冲里（不碰 data）。调用方把它放在结果的**开头**。 */
void edc_output_buf_truncation_note(const struct edc_output_buf *b, char *out, size_t out_cap);
void edc_output_buf_free(struct edc_output_buf *b);
void edc_output_buf_size_str(size_t n, char *buf, size_t cap);   /* 812 B / 1.0 MiB / 3.2 GiB */
```

为什么值得抽成模块：**纯数据、无 I/O、无 libuv → 可以直接单元测试**。现有工具逻辑难测，恰恰因为它长在 event loop 上；把"限额"这一层从 loop 上摘下来，是这次改动最容易验证的部分。

### 3.2 shell：接上累加器 + 两档阈值

读回调改成 `edc_output_buf_append(&ctx->out, buf->base, nread)`（stdout/stderr 各一个），再加两个常量（放在 `CMD_TIMEOUT_SEC` 旁边，同风格）：

| | 建议值 | 行为 |
|---|---|---|
| **Tier 1 捕获上限** `EDC_OUTPUT_CAPTURE_MAX` | **1 MiB / 流** | 到顶后**继续读、继续 `free`、不再存**：管道不会被写满，合法命令照常跑完；每调用内存恒定（2 MiB + 结果拷贝 ≈ 4 MiB），与输出量无关 |
| **Tier 2 跑飞上限** `EDC_OUTPUT_HARD_MAX` | **256 MiB / 单流** | 超过就 `kill(-pid, SIGKILL)`（复用 timeout / abort 同一套杀法），记 `over_limit` |

结束时两种文案分开写清（数字都从实际值来）：

```
[output truncated: kept the first 1.0 MiB of 3.2 GiB]          ← 只截断，命令正常跑完
[output exceeded 256 MiB — killed; kept the first 1.0 MiB]      ← Tier 2 触发
```

**为什么两档**：只留 Tier 1，`!yes` 内存安全但**永不结束**（任务一直占前台）；只留 Tier 2 就是 CC 的做法（它在 **5 GB** 才杀），一个正常 verbose 的 build（几 MB 日志）会被误杀。分开之后各治一个病。

**副作用是白拿的**：由 §1.2 的结论，捕获层一限住，**TUI 气泡与 chat.jsonl 同时有界**，不需要另外改。

### 3.3 顺手修

- `tool_max_output_size` 的提示改为用配置值（保留 total 数字）。
- 手册里补一句区分："给模型的工具结果会被截到 `tool_max_output_size`；界面上看到的是捕获到的内容"。

### 3.4 分阶段（避免一次改太多）

- **P1（本方案主体）**：`edc_output_buf` + shell 两档阈值 + 文案 + 单测 + 端到端测量。
- **P2**：`device_shell` / `device_file` 的 `resp_buf` 与下载 body 接同一个累加器（它们已有自己的 timeout，不另加 Tier 2）。
- **P3**：`read_file` / `edit_file` 加"文件过大就拒绝/截断"的前置检查（读整文件是同类 OOM）。
- **P4（可选）**：Tier 1 到顶时**溢出到 workspace 文件**（opencode 的做法）——结果里给"路径 + 预览"，被丢弃的大输出还能找回来。要处理文件命名与清理策略，单独一轮。

## 4. 验证

- **单测（新，最便宜且最该有）** `tests/test_output_buf.c`：恰好到顶 / 略超 / 极超（total 继续涨但 data 不再涨）/ `truncated` 标志 / `finalize` 文案里的数字。
- **集成测试（补上缺的那个骨架）** `tests/test_tool_shell_loop.c`：真 `uv_loop` + 真 toolbox actor + 假 LLM。两条命令：
  - `seq 1 10000000` → 断言 `data` 长度 == cap、`total` 正确、**命令正常跑完**（没有被杀）；
  - `yes` → 断言 Tier 2 触发、进程确实死亡、文案是"killed"那一条。
  这同时把"唯一没被 `bazel test` 覆盖的层"补上，让第 5 条变成永久回归。
- **pty 端到端**：`!yes` 跑 ~10 秒，采样 `/proc/<pid>/status` 的 `VmRSS`，断言**不随时间上涨**；再断言任务最终以"被杀"的提示收敛。

## 5. 待定

1. 数值：`1 MiB / 256 MiB` 可以吗？（内存恒定，与输出量无关；这两个值只决定"能保留多少"和"多疯算疯"）
2. ~~常量还是配置项？~~ → **已定：编译期常量作默认值，同时挂在 `edc_config` 上**（`shell_capture_max` /
   `shell_output_kill_max`，0 = 用默认）。刻意**不**写进 config.json：它们的用途是"让测试能钉一个小额度"
   （否则测 256 MiB 的跑飞就得真吐 256 MiB）。要变成用户可配只是两行的事。
3. P1 要不要带 P2（device）一起做？倾向分开：先把用户命令这条高风险面闭环。

---

## 6. 实现记录（已落地）

### 与批准方案的偏差（一处，已按"默认值不变"处理）

两个阈值按批准的是**常量**，但额外挂了两个 `edc_config` 字段（`shell_capture_max` /
`shell_output_kill_max`，0 = 用常量默认值），**不进 config.json**。原因：集成测试要能钉一个小额度，
否则"跑飞被杀"这条测试得真吐 256 MiB 才能触发。生产行为与批准的完全一致（默认 1 MiB / 256 MiB）。

### 端到端验证时抓出来的两个真问题

1. **提示放错了地方**：最初把"kept the first X of Y / command killed"追加在结果**末尾**。真实跑
   `!yes` 时两个读者都看不到它——TUI 气泡**折叠**只画开头几行，给模型的那份又在
   `tool_max_output_size` 处**从头部**裁断。于是提示恰好活在最没人看的地方。
   → 改成**拼在结果开头**，并加断言钉住（"提示必须出现在前 200 字符内"）。
2. **`1024.0 KiB` 而不是 `1.0 MiB`**：保留量是 1 048 570 字节（差几十字节没到 1 MiB），`%.1f`
   把它四舍五入成 `1024.0 KiB`。→ 进位阈值改成"**一旦会四舍五入到 1024.0 就进上一档**"，
   并加了两条边界断言（1048570 B → `1.0 MiB`，1023 B → `1023 B`）。

### 验证

- **单测** `tests/test_output_buf.c`：空/未满/正好在顶/单次跨顶/超顶后继续涨/cap 0/文案数字/单位换算
  （含上面那个 `1024.0 KiB` 边界）。
- **集成测试** `tests/test_tool_shell_loop.c`（**补上了那个一直缺的骨架**：真 `uv_loop` + 真
  `edc_toolbox` + 真 `exec_shell`，loop 由测试线程 `uv_run(UV_RUN_NOWAIT)` 泵）：
  普通命令不受影响 / 洪亮但有限的命令只截断不被杀（且提示在开头）/ `yes` 被跑飞上限杀掉
  （**调用能返回本身就是证据**——`yes` 永不自己退出）/ 同样的量走 stderr 也一样被杀。
- **pty 端到端**（真二进制、沙箱关）：`!yes` 让命令吐完 **256.1 MiB**，任务正常收敛为
  `DONE/COMPLETED`，而 app 的 `VmRSS` **峰值 11.0 MB**（基线 10.9 MB）；`chat.jsonl` 里
  工具气泡的**第一行**就是
  `[output truncated: kept the first 1.0 MiB of 256.1 MiB]` +
  `[output exceeded 256.0 MiB — command killed]`。
  顺带量到：256 MiB 的 `yes` 约 **4.9 秒**被杀死。
- 全量 `bazel test //tests/... --nocache_test_results` → **76/76**（原 74 + 新增 2）。

### 仍未做（P2/P3/P4）

`device_shell` / `device_file` 的 `resp_buf` 与下载 body、`read_file` / `edit_file` 读整文件，
以及"到顶溢出到文件"——本次范围之外（用户已明确"不带 device"）。

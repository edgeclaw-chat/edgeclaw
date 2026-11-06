# 工具输出的上限：一个累加器，三层各有说法

> 状态：**部分实现（2026-10-22 更新）** —— 第 2 节的目标 1（源头限内存）与 3（如实说明）已落地；
> 本文第 1 节描述的是**实现前**的现状，保留作为背景。当前行为见 §1b（输出）与 §1c（读取）。
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

## 1b. 现在的行为（2026-10-22）

工具结果出给调度器时，命中**两个上限之一**就外溢，**不再丢弃**：

- 字节：`tool_max_output_size`（配置，默认 10000）
- 行：`EDC_TOOL_OUTPUT_MAX_LINES` = 2000

命中后：

1. 完整结果写进 `<workspace>/<session>/tool-output/<tool>-<seq>.txt`
   （`edc_file_write_safe`，root = workspace —— 模型能写这棵树，不防 symlink 就等于给它一个改写 config 的机会）；
2. 消息变成「**头部**通知 + 有界预览」：

```
[truncated: showing 312 of 4000 lines (10000 of 18893 bytes)]
[full output: /…/workspace/s1/tool-output/exec_shell-1.txt (read it with read_file offset/limit)]
<前 10000 字节，切在行边界>
```

**为什么通知在头部**：预览是从头裁的，放尾部的通知正好是被裁掉的那一段。
`grep`/`find` 的 cap/timeout 提示因此也一并从尾部移到头部 —— 它们原本**永远到不了模型**
（自身的 64 KiB 预算先收集，随后被 10000 字节的通用上限连通知一起切掉）。

声明的 `keep_full_output` 的工具（`read_file`、7 个 `memory_*`）不受此限：它们自己按行/条目
封顶，且清楚自己输出的是什么。

**外溢文件的生命周期**：挂在 session 目录下，随 session 删除而消失；不落 `/tmp`（无人拥有、
永不清理）。文件名带工具名与序号，同一次运行内不覆盖。

**已验证（端到端）**：模型跑 `seq 1 4000` → 收到截断通知与路径 → 用 `read_file offset` 读回
外溢文件 → 报出最后一行 `4000`。

---

## 1c. 读取侧的输入上限（2026-10-22）

上面说的是「输出」；**输入**也有一条同类的洞，并且同一天补上了。

`edc_file_read()` 是全仓 20 处共用的读文件原语，**没有上限**：先 `fstat` 拿到大小，然后
`malloc(size+1)`。文件大小若来自模型（`read_file` 的 `path`）或用户（`@file` 粘贴），
那这行 `malloc` 就是一次不可控分配 —— 读一个 10 GB 的文件就是 10 GB 的内存。

新增 **`edc_file_read_capped(path, max_bytes, &out_size, &err)`**：**先 stat、后分配**，
超限直接拒绝，并把**真实大小**放进错误里（拒绝时也填 `out_size`），调用方因此能说
"它多大 + 你该怎么办"，而不是给出一个笼统的失败。

接入的地方（都是「大小不由本进程决定」的读取）：

| 位置 | 上限 | 超限时的说法 |
|---|---|---|
| `read_file` / `edit_file` | **8 MiB**（单次读取） | `read_file`：给出字节数与替代做法（`exec_shell` 的 `sed -n`/`head`/`tail`）；`edit_file`：说明 `old_text` 必须对全文匹配，**不能**分片，改用 `sed`/`perl` |
| `grep` / `find` 的逐文件读 | 8 MiB（`EDC_SEARCH_MAX_FILE_BYTES`） | 静默跳过（搜索结果里不该因为一个巨型文件就报错） |
| `@file` mention | 8 MiB | 走既有的失败块，正文里给出原因 |

`read_file` 的 8 MiB 与它自己 64 KiB 的**返回窗口**是两件事：窗口管"给模型多少"，
8 MiB 管"进程愿意读多少"。前者可以用 `offset` 一点点搬完一个大文件，后者是硬墙。

**已验证（端到端）**：100 MB 文件 —— `read_file` 与 `edit_file` 都在**分配之前**被拒，
进程内存只涨 6 MB（纯启动开销），两条报错都带真实字节数与下一步。

**仍然没有上限的地方**（同一类问题，见 §1 末尾）：`device_*` 的 `resp_buf`、device 下载
把整个 body 攒在内存里。

---

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
- **P4（可选）**：Tier 1 到顶时**溢出到 workspace 文件**——结果里给"路径 + 预览"，被丢弃的大输出还能找回来。要处理文件命名与清理策略，单独一轮。

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


---

## 7. 输出编码：工具结果必须是合法 UTF-8（Windows 需要动手，POSIX 不动）

> 落地：`10ce393`（spawn 侧 prologue）、`965438e`（输出边界归一化）、`fc55164`（BOM 元数据化）。
> 触发：Windows 上 `exec_shell` 返回的中文是乱码；`Get-Content` / `Set-Content` 会静默读错、写错。

### 7.1 一条不变式

**任何工具结果都是合法 UTF-8。** TUI、`read_file`、以及给模型的那一份（按 `tool_max_output_size`
从**头部**裁断）全都建立在这条上，而且事后无法从字节反推它是否成立，所以只能在出口保证一次。

Linux / macOS 上这条本来就成立（`sh -c` 只透传字节），因此 POSIX 侧**一行都不改**：
`edc_platform_output_to_utf8()` 在那一边返回 `NULL`（意思是"没有要转的东西"），并且
**故意不做校验**——`cat` 一个二进制文件、或跑一个仍输出 GBK 的老工具，必须原样返回那些字节。
在那里"帮忙"校验一次，就是把行为变更伪装成修 bug。

### 7.2 两个机制，各治一段（不是二选一）

| 机制 | 修得到的 | 修不到的 |
|---|---|---|
| prologue（`shell_prologue`，spawn 时拼在命令最前） | PowerShell 自己的文本；**以及共享同一 console 的 cmd 内建命令**（实测 `cmd /c echo 中文` 变正确） | 按 CRT locale 编码的程序：python、gcc、本地化系统工具（实测 `chcp 65001` 与 `[Console]::OutputEncoding` 都不动它们） |
| 边界归一化（`shell_ctx_complete` 里逐行） | 上面这一类 | 同一行内混排两种编码（按行判定，一行只走一条路） |

prologue 的三句各修一条边界，缺一句就漏一条：

- `[Console]::OutputEncoding` —— PS 自己的输出，顺带把 console 码页也改了；
- `$OutputEncoding` —— PS 管道给原生程序时用，默认是 **us-ascii**，非 ASCII 直接丢；
- `$PSDefaultParameterValues['Get-Content:Encoding' | 'Set-Content:Encoding' | 'Out-File:Encoding']`
  —— 文件读写的隐式编码。没有它，`Get-Content f | Select-String 匹配` 会对一个**确实包含**该词的文件回答
  "没匹配到"：那是**错误答案**，不是显示问题。用**具名 cmdlet** 而不是 `*:Encoding`，后者会顺带改
  `Export-Csv` / `ConvertTo-Json`。

`-NoProfile` 不是洁癖，是**时序**：profile 只要往 stdout 写一个字节，编码就在 prologue 之前被钉死了。
`-File` 走不通——被执行策略挡（`UnauthorizedAccess`），prologue 只能是 `-Command`。

### 7.3 结果头部的 `[encoding: ...]` 是什么意思

```
[encoding: 2 line(s) converted from cp936 to utf-8]
```

这条结果里有 N 行原本不是 UTF-8（GBK / 本地 ANSI 码页），已经被转成 UTF-8。**转码是我们改动了输出**，
所以明说而不是悄悄做：查编码问题的人需要知道哪几行不是原始字节，这也是这套机制唯一的可观测点。

判定是**每行三档**：合法 UTF-8 → 原样保留；不合法但 ANSI 码页合法 → 转码；两者都不合法
（二进制，或被捕获上限切成半个字符）→ 每个坏字节换一个 U+FFFD，**结果是合法 UTF-8 且损坏可见**，
而不是编出一段看着合理的文字。

严格性是这条链的命门：宽松解码器会把一行 GBK 当成一串拉丁字符"接受"下来，于是转码永远不触发，
中文就一直是乱码。所以 overlong（`C0 80`）、代理对（`ED A0 80`）、超范围（`F4 90 80 80`）全部拒绝
（`src/common/edc_utf8.h`）。

被捕获上限截断的那条流，最后一行可能停在半个字符上：该尾巴**直接丢掉**而不是拿去转码——转半个字符
会得到一个"看着像内容"的尾巴，而截断提示已经说明尾巴缺失了。

### 7.4 BOM：Windows 上算元数据

prologue 生效之后，`Set-Content` / `Out-File` / `>` 写出的文件是 **UTF-8 带 BOM**（`ef bb bf`；
`>` 原本是 UTF-16LE 的 `ff fe`）。`read_file` 原样返回就会多一个不可见的 U+FEFF，并且会让 `edit_file`
的首行匹配对着一个"看起来一模一样"的字符串失败。所以工具边界剥掉它
（`edc_platform_bom_is_metadata()`：Windows true / POSIX false），`edit_file` 写回时再补上——
**一次编辑不该改动它没被要求改的字节**。

剥的位置是**工具边界**，不是 `edc_file_read()`：后者还给 config 加载和 session 状态文件用，
那里的字节在各平台上必须一字节不差。UTF-16LE 的 `ff fe` 不剥：那说明文件根本不是 UTF-8。

### 7.5 排查时必须知道的两条

1. **同一个 console 内，编码是粘性的。** `chcp` 一次之后，同一 console 的后续子进程都会变，
   于是同一个探针会给出前后矛盾的结论。**验证必须每次新 spawn**（edgeclaw 每次 `exec_shell` 都有独立
   console，所以改动不会串到并发调用——但你的实验会串）。
2. **`powershell.exe -File script.ps1` 被执行策略挡。** 想在实验里复现 prologue，用
   `-EncodedCommand` 绕开 PS 调用原生程序时的引号改写。
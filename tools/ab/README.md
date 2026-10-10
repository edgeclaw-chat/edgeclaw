# A/B 对照夹具（`tools/ab`）

**目的**：证明"**对齐现有实现**"（这是本仓的硬规则）。
测试有洞 —— 宿主上固定 6 个失败，其中 `test_llm_actor` / `test_https_client_get`
**正好盖住 P1 要动的 LLM 路径**。所以需要一条独立于测试的证据：

> 同一份夹具 + 同一串输入 → 两个二进制产生**相同的产物**。

---

## 为什么能覆盖 LLM 回路（勘察结论）

| 事实 | 出处 | 推论 |
|---|---|---|
| 未配 `cacert_path` 时 `authmode = VERIFY_NONE` | `edc_https_client.c:717-730` | **本地自签 TLS 服务器可直接用**，不需要 CA |
| 响应是普通 JSON `choices[0].message`，**非流式** | `edc_llm_actor.c:263-345` | 假 LLM 只需返回一段固定 JSON |
| `api_url` 完全可配（含端口） | `config.json` | 指向 `https://127.0.0.1:8443/...` |
| `api_key` 用明文（无 `EDGECLAW_ENC:` 前缀）时无需解密 | `edc_secret` 的加密前缀机制 | **无需口令，可全自动运行** |

所以夹具能跑**完整回路**：用户输入 → 假 LLM（固定 `tool_calls`）→ 真工具执行 → 假 LLM（固定回复）→ 落盘。

---

## 目录

```
tools/ab/
  README.md            本文件
  fake_llm.py          自签 TLS + 固定响应序列
  ab.py                驱动：run / compare / selftest / gate
  scripts/*.txt        输入序列（DSL 见下）
  responses/*.json     假 LLM 的响应序列
```

夹具**不入库**：运行时从一份真实工作区**冻结**出来 —— 既把大文件挡在仓库外，
又保证"两个二进制跑同一份数据"（历史上 `/tmp/e2e-frozen` 的手法）。

---

## 输入序列 DSL（`scripts/*.txt`）

```
# 注释
wait 800              # 等 800ms
send /task list\n     # 发送（支持 \n \r \t \xNN）
resize 100x40         # 改终端尺寸
```

### 夹具清单

| 夹具 | 覆盖 |
|---|---|
| `smoke` | 命令路径 + 完整 LLM 回路（含一次真工具执行） |
| `bg` | 后台任务 + 快照推送 |
| `cancel` | 在途取消（`/task stop` + `__delay` 制造在途窗口） |
| `fsok` / `fifo` / `timeout` / `smoke5` | 文件工具/超时等零散场景 |
| **`fold`** | **L1 折叠**：跑一个任务后手动 `/group fold`（见下），折一次真的折叠 |
| **`switch`** | **切台**（P3 ①b）：`/session new` → 在新台说话 → `/session switch` 切回。钉住"切 session 的用户可见结果不变"：两个 session 各自成套（`session.json` / `chat.jsonl`），那条 "**Switched**" 回复仍落在**旧** session 的转录里（顺序 = 先回复、后换台事件），后台任务继续跑 |
| **`seqfail`** | **`;` 链上"上游非完成地终结"**：`first ; second` + 首个应答压后 60s + `--env EDGECLAW_HTTPS_TIMEOUT_MS=1500` → 上游超时进 ERROR → **下游必须被驱动**（2026-09-18 "就绪后启动"那条路；五条老夹具都不含 `;`，打不到它）。跑法要带 env：`run --fixture seqfail --env EDGECLAW_HTTPS_TIMEOUT_MS=1500` |

`fold` 夹具要证的：折叠从 `memory_actor` 的专用协议搬成 system task 之后，
`session.json`（memory 文本 / `turns_folded` / 折叠水位）、`chat.jsonl`、
`llm_requests.jsonl` 归一化后不变（raw 只差 system prompt 里的 clock）。唯一**固有**的差异是 `.task_seq` +1 ——
折叠现在真的是一个任务，它要一个 id（id 不能复用，重启后必须越过它）。

**冻结门槛**：折叠的触发线是 `context_window - 16384`（默认窗口 1M），由
`AB_CONTEXT_WINDOW` 环境变量决定（默认 1048576），写在 **pristine 的 config.json** 里，
所以**只在冻结那一次生效** —— 要重冻：`rm -rf /tmp/ab/run/fold`。夹具的语料太小，
在 1M 窗口下自动路径不会过线，所以 `fold` 夹具用手动 `/group fold` 触发；自动触发由
`tests/test_group_fold.c` 的挂载点用例覆盖（见 `scripts/fold.txt` 的说明）。

## 假 LLM 响应序列（`responses/*.json`）

按**请求顺序**消费；用完后重复最后一个（保证确定性）：

```json
[
  {"choices":[{"message":{"role":"assistant","content":"好的"},
               "finish_reason":"stop"}]},
  {"choices":[{"message":{"role":"assistant","content":null,
                          "tool_calls":[{"id":"call_1","type":"function",
                             "function":{"name":"shell","arguments":"{\"command\":\"echo hi\"}"}}]},
               "finish_reason":"tool_calls"}]}
]
```

---

## 产物清单（比什么、**不比**什么）

| 产物 | 比法 |
|---|---|
| `workspace/<sid>/session.json` | 归一化后一致（见下）—— raw 差 clock（`last_active` / 消息 `ts`） |
| `workspace/<sid>/chat.jsonl` | 掩 clock 后一致；实测 raw 也相同（转录不带时刻） |
| `workspace/sessions.json` | 归一化后一致 |
| `workspace/.task_seq` / `.session_seq` | **逐字节**（数字，无时钟） |
| `llm_requests.jsonl` | 掩 clock 后一致 —— raw 差 system prompt 里的 `## Current Time` 与请求 `ts` |
| `log/*.log` | 去掉时间戳后逐行 |
| `session.json.broken` | **不应存在** —— `run` 与 `compare` 都有**闸门**：`artifacts/` 下出现 `*.broken`、或 **`_log/` 下任何文件**（含轮转过的 `.log.1`）出现 `quarantin*` → **直接失败**（退出码 **4**），不进入比对（两边可能同样残缺，那样 compare 会说"一致"）。闸门自己的回归见 `ab.py gate` |
| **终端帧** | **不比** —— 见下 |

### 归一化（非确定性的来源）

归一化 = `canon()` 对**每个**产物做的三件事：

1. **掩时钟**（`TS_RE`，`YYYY-MM-DD HH:MM:SS`）：`session.json` / `sessions.json` 里的
   `created_at` / `last_active`、消息的 `ts`、system prompt 里的 `## Current Time` —— 两次运行必然不同；
2. **掩行号**（`LINE_RE`，`file.c:NNN`）：往任何会打日志的文件里加一行，日志里的行号就位移，
   那是编辑的副作用、不是行为（实测：`edc_https_client.c` 加 55 行 → `724` 变 `779`）；
3. **`.json` 再规范化**：解析 → 掩掉 1 里的字符串 → 按键序重序列化。

所以文档里说的"一致"= **归一化后一致**；`chat.jsonl` / `.task_seq` 是少数 raw 也相同的产物。
**唯一抹不掉的产物差异**是 `.task_seq`（id 高水位，见 `fold` 夹具）。

### 为什么第一版不比终端帧

TUI 有动画（呼吸点、spinner），且 ftxui 只重绘 diff（记过：
pty 字节重放只能看到前几帧）→ **帧比较不可靠**。
渲染本身已由 `//src/tui:all` 的 **10 个测试**覆盖；A/B 专注**状态机产物**。

> 将来若要比帧：用 `resize` 强制整屏重绘，取 **settle 后的最后一帧**。

---

## 用法

```bash
# 1. 两个二进制
bazel build //src/tui:edgeclaw_tui
cp bazel-bin/src/tui/edgeclaw_tui /tmp/ab/bin/a      # 改动前
# ... 改动 ...
cp bazel-bin/src/tui/edgeclaw_tui /tmp/ab/bin/b      # 改动后

# 2. 对照（夹具运行时从真实工作区冻结；默认 source=~/.edgeclaw/workspace, sessions=s8）
python3 tools/ab/ab.py run --bin /tmp/ab/bin/a --label a --fixture smoke
python3 tools/ab/ab.py run --bin /tmp/ab/bin/b --label b --fixture smoke
python3 tools/ab/ab.py compare --a /tmp/ab/out/a --b /tmp/ab/out/b

# 3. 自证确定性（**必做**）
python3 tools/ab/ab.py selftest --bin /tmp/ab/bin/a --fixture smoke

# 4. 自证**闸门**还在（不跑二进制，秒级）
python3 tools/ab/ab.py gate
```

**`selftest` 不过 = 夹具本身不确定 = A/B 的结论无效。** 这是使用夹具的前置条件。
同理，若运行**没有任何 LLM 请求**，`run` 会以退出码 3 失败 —— 那说明 LLM 回路没被覆盖。
`gate` 验的是**闸门自己**（造脏物断言判得对）—— 它是给下面那条"脏产物"规定的回归：
C 侧的隔离日志文案一改、`_log/` 布局一变，闸门就会静默失效，而那种失效**看起来像"一致"**。

### 落地细节（实测）

| 事项 | 做法 |
|---|---|
| 免口令 | 子进程 env 里设 **`EDGECLAW_MASTER_KEY`**（`edc_secret.c:193` 的 "Priority 1: env var"），跳过口令提示，也不需要 `.key_hash` |
| 临时 HOME | `uv_os_homedir()` **先看 `$HOME`**（`edc_file_utils.c:374`）→ 夹具 HOME 生效 |
| api_key | 明文（无 `EDGECLAW_ENC:` 前缀） |
| 沙箱 | `rw_paths` 自动包含各会话的 `session.workdir`（否则工具被拒） |
| 路径稳定性 | HOME 路径**固定**为 `/tmp/ab/run/<fixture>/home` —— 否则 config 里的沙箱路径会渗进 system prompt，破坏可比性 |
| 工具名 | 是 **`exec_shell`**，不是 `shell`（夹具第一次跑就把这个抓出来了） |

产物在 `/tmp/ab/out/<label>/`：`artifacts/`（工作区 + 日志）、`llm_requests.jsonl`、
`screen.bin`（仅留证，**不参与比较**）。

### 退出码（脚本/CI 要看的）

| 命令 | 码 | 含义 |
|---|---|---|
| `run` | 0 | 这次运行干净 |
| | 3 | 一个 LLM 请求都没发生 → 回路没被覆盖，结论无效 |
| | 4 | 产物里有**隔离痕迹**（`*.broken` / 日志 `quarantined`）→ 不干净 |
| | 其它 | 被测二进制自己的退出码（透传） |
| `compare` | 0 / 1 | 一致 / 有差异 |
| | 4 | 任何一侧产物不干净 → **不比对** |
| `selftest` | 同 `run` | 某一次自证运行不干净时立即失败（不再假装"一致"） |
| `gate` | 0 / 1 | 闸门自测通过 / 闸门自己坏了（先修它，再看任何 A/B 结论） |

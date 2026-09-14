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
  ab.py                驱动：run / compare / selftest
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
| `workspace/<sid>/session.json` | **归一化**后逐字节（见下） |
| `workspace/<sid>/chat.jsonl` | 逐字节 |
| `workspace/sessions.json` | 归一化后逐字节 |
| `workspace/.task_seq` / `.session_seq` | 逐字节 |
| `log/*.log` | 去掉时间戳后逐行 |
| `session.json.broken` | **不应存在**（出现即失败） |
| **终端帧** | **不比** —— 见下 |

### 归一化（非确定性的来源）

`session.json` / `sessions.json` 里有**写入时刻**（`created_at` / `last_active` 等），
两次运行必然不同。归一化 = 解析 JSON → 把这些字段换成固定占位 → **确定性重序列化**。

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
```

**`selftest` 不过 = 夹具本身不确定 = A/B 的结论无效。** 这是使用夹具的前置条件。
同理，若运行**没有任何 LLM 请求**，`run` 会以退出码 3 失败 —— 那说明 LLM 回路没被覆盖。

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

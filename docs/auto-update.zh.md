# 自动更新（C 档：检测 → 弹窗 → 一键更新）

> 状态：**设计稿，未实现**
> 目标读者：改 `src/common`（更新/替换）、`src/tui`（确认框）、`.github/workflows/release.yml`（产物）的人
> 修订：
> - v1：完整自替换
> - **v2（本稿）**：按维护者决策收敛 —— 检查**每次启动只跑一次**（不周期轮询）；**必须签名**；
>   默认开启；**只提示不自动重启**；**Windows 一并支持**。

## 0. 已定决策

| 决策 | 取值 | 对设计的影响 |
|---|---|---|
| 检查频率 | **每次启动一次**，不周期 | 不需要 `last_check` 持久化、不需要周期服务、不需要 ETag 缓存；限流（60/h）不再是问题，但仍要退避 |
| 签名 | **必须**，缺失即拒绝更新 | 引入 ECDSA P-256 验签；`sha256sums.txt` 从"校验和"升级为"被签名的载荷" |
| 默认开关 | `update_check: true` | 首次运行必须告知（§10） |
| 更新后 | **提示，不自动重启** | 不需要 `execv`；用户自行重启 |
| Windows | **支持** | 需要"改名 + `.old` 清理"路径（§8.3） |

## 1. 目标与非目标

**目标**：用户在 TUI 里收到「新版本可用」的提示，确认一次，二进制被替换成新版本，**重启后**生效。不需要用户知道 `install.sh`、不需要手动下载。

**非目标**（明确的边界，见 §12）：

- 不支持降级。
- 不在二进制为 `-dirty`/`dev` 时更新自己。
- 不在安装目录不可写时尝试。
- 不静默替换——每次替换都必须有一次用户确认。
- 不自动重启——替换完成后只提示。

## 2. 信任链：签名是硬要求

C 档的含义是：**程序替换自己的可执行文件**。所以下载链必须能回答"这就是官方产物"，而不只是"TLS 没被中间人劫持"。

| 手段 | 防得住 | 防不住 |
|---|---|---|
| 只有 TLS | 被动监听 | 发布流水线被入侵、GitHub 侧被改 |
| `sha256sums.txt` | 传输损坏、拿错平台、缓存串包 | 修改产物**同时**修改校验和 |
| **校验和 + 签名**（本稿） | 以上全部 | 签名私钥泄露 |

**本稿的实现选择：ECDSA P-256（secp256r1）+ SHA-256。**

选它而不是 minisign/Ed25519 的理由：仓库已启用 `MBEDTLS_ECDSA_C` 与 `MBEDTLS_PK_C`，
而 `MBEDTLS_ED25519_C` 未启用（且 Ed25519 走 PSA，需要开 `MBEDTLS_PSA_CRYPTO_C`，
是一次更大的配置改动）。用现有的 ECDSA 意味着**零新依赖、零构建配置变更**。

**签名对象**：`sha256sums.txt` 的**原始字节**。签名文件 `sha256sums.txt.sig` 存放
base64 编码的 ASN.1 DER 签名。公钥以 C 数组形式内嵌在二进制里（`edc_update_pubkey.c`），
这样"信任锚"随二进制一起分发，没有单独的可替换文件。

**校验顺序（不可调换）**：

```
1. 下载 sha256sums.txt            → 内存
2. 下载 sha256sums.txt.sig        → 内存
3. 用内嵌公钥验签 sums.txt         → 失败即中止，不再往下
4. 从(已验证的) sums.txt 里取出本平台产物的期望 sha256
5. 下载归档，边下边算 sha256
6. 与(可信的)期望值比对
```

**先验签、后比对**的顺序是关键：如果先比对再验签，攻击者可以同时改产物和校验和，
比对会通过。

**签名缺失的处理**：`sha256sums.txt.sig` 不存在或验签失败 → **拒绝更新**，提示用户手动更新。
理由直接抄 `install.sh` 已有的原则：*"an install script that silently skips verification
once a checksum exists is worse than one that never claimed to."*

**密钥管理**（需要你来做，我做不了）：

```bash
# 一次性：生成签名密钥（私钥绝不能进仓库）
openssl ecparam -name prime256v1 -genkey -noout -out edgeclaw-signing.key
openssl ec -in edgeclaw-signing.key -pubout -out edgeclaw-signing.pub.pem
# 私钥进 CI secrets（UPDATE_SIGNING_KEY），公钥提交进仓库
```

我会提供 `tools/gen_pubkey_c.py` 把 PEM 公钥转成 `edc_update_pubkey.c` 里的 C 数组，
并在 CI 里加签名步骤。**在真实公钥替换进来之前，验签会恒定失败**——这是刻意的安全默认值：
占位公钥下所有更新都被拒绝，而不是放行。

## 3. 状态机

```
        ┌──────────┐  启动后延迟 10s，只一次
        │   IDLE   │────────────────────────────┐
        └──────────┘                            ▼
             ▲                            ┌───────────┐
             │  失败/超时/限流             │ CHECKING  │
             │                            └─────┬─────┘
             │                      更新可用    │   无更新/失败
             │                                  ▼
             │                            ┌───────────┐
             └────────────────────────────│  NOTIFIED │ 横幅（非模态）
                                          └─────┬─────┘
                                        用户按 /update 或点击
                                                ▼
                                          ┌───────────┐  用户取消
                                          │ CONFIRMING│──────────► IDLE
                                          └─────┬─────┘
                                             确认
                                                ▼
                                    ┌───────────────────────┐
                                    │ DOWNLOADING → VERIFY  │  有进度，可取消
                                    │ → STAGING → REPLACING │
                                    └───────────┬───────────┘
                                      失败      │      成功
                                        ▼       ▼
                                  ┌────────┐ ┌──────────┐
                                  │ FAILED │ │ RESTART  │ 提示重启（可立即重启）
                                  └────────┘ └──────────┘
```

**关键性质**：`REPLACING` 之前的每一步都可以安全中止/重试；`REPLACING` 是一个原子 `rename()`，没有中间态。

## 4. 组件与落点

### 新增

| 文件 | 职责 |
|---|---|
| `src/common/edc_semver.{c,h}` | 版本比较（含 prerelease 规则） |
| `src/common/edc_update.{c,h}` | 检查（HTTP+JSON）、下载（流式落盘）、sha256 校验、状态机 |
| `src/common/edc_selfreplace.{c,h}` | 自身可执行路径、原子替换、平台分支 |
| `src/common/edc_targz.{c,h}` | 最小 tar+gunzip 读取（仅取单一文件，见 §7） |
| `src/kernel/scheduler/edc_update_service.c` | 注册 `update-check` 服务，把结果 Post 给 UI |
| `src/tui/edc_update_dialog.{hpp,cpp}` | 模态确认框 + 进度展示 |

### 改动

| 文件 | 改动 |
|---|---|
| `.github/workflows/release.yml` | 生成并上传 `sha256sums.txt` |
| `tools/install.sh` | 下载后校验（现在没有） |
| `src/common/edc_config.{c,h}` | 新增 `update_check`（默认 on）、`update_channel`（`stable`/`pre`） |
| `src/common/edc_platform.{c,h}` | 新增 `edc_platform_exe_path()` |
| `src/tui/edc_tui_window.cpp` | 挂确认框、`/update` 命令、重启入口 |
| `src/shell/edc_commands.c` | 命令表登记 `/update` |
| `docs/edgeclaw.md`、`docs/edgeclaw.1`、`docs/limitations.md` | 文档与已知限制 |

### 依赖决策：gzip（唯一需要新增的依赖）

仓库**没有** zlib/gzip（`MODULE.bazel` 里没有，mbedtls 也不提供 gzip）。而发布产物是 `.tar.gz`。三个选项：

| 选项 | 代价 | 评价 |
|---|---|---|
| **A. 加 zlib + 自写最小 tar 读取**（推荐） | 新增一个 bazel_dep；~200 行 tar 解析 | 单一产物，与 `install.sh` 同一份归档；zlib 极稳定 |
| B. 额外发布未压缩的原始二进制资产 | 产物翻倍（每平台两个文件）；下载体积 ~8MB vs ~3MB | 零新依赖，但两份产物必须同步，容易不一致 |
| C. 调用系统 `tar` | 静态二进制要有外部依赖 | 与"自包含"定位冲突，否决 |

**推荐 A**，理由：归档内容（`LICENSE`、`THIRD_PARTY_NOTICES.md`、`licenses/`）与可执行文件必须来自同一次构建；选项 B 引入了"两份产物可能不同步"的新故障模式。

## 5. 检查（每次启动一次）

**端点**：`GET https://api.github.com/repos/edgeclaw-chat/edgeclaw/releases`（不用 `/releases/latest`——它会跳过 prerelease，而当前所有 tag 都是 prerelease）。

**一次一个进程**：检查在启动后**延迟若干秒**跑一次，然后这个进程再也不查。没有周期服务、
没有 `last_check` 持久化、没有 ETag 缓存——因为决定就是"等用户下次启动再查"。

这样做的连带好处：GitHub 未认证 API 的 **60 次/小时/IP** 限制不再是问题（一个进程只花一次），
但仍要处理 403（我实测撞到过），退避即可。

**时序**：

```
启动 → TUI 首屏可交互 → 延迟 10s（不和启动时的模型请求抢）→ 检查一次 → 结果 Post 给 UI
```

**检查本身的护栏**：

- 超时 10s（`edc_https_client` 的连接超时）
- 失败静默（只有 `log_debug`），绝不产生用户可见噪音
- 不重试
- 结果只在本进程内存里，不落盘

**选择逻辑**：

```
候选 = releases 里 draft=false 的项
若 update_channel == "stable"：只取 prerelease=false
若 update_channel == "pre"   ：全部（当前发布都是 prerelease，见 §10）
取 semver 最大者，且必须 > 当前版本，且必须带本平台资产（归档 + sums + sig）
```

**当前版本**取自 `edc_version()`。若等于 `dev`/`unknown` 或含 `dirty`，**整条链路直接禁用**（§12）。

**Windows 的平台判定**：资产名形如 `edgeclaw_amd64_windows_<tag>.tar.gz`。判定要在编译期而非
运行期做错了会下载错平台——所以用编译期宏选资产名模式，和 `tools/install.sh` 的 `target` 判定
保持同一张表。

## 6. 下载与校验

```
1. GET <asset>.tar.gz，流式写入 ~/.edgeclaw/updates/<tag>.tar.gz.part
   每次 write 同步更新 sha256 上下文（边下边算，省一次全文件读）
   带 Content-Length 校验；有硬上限（比如 64 MB）防异常大包
2. GET sha256sums.txt（已缓存则不重复请求）
3. 比对；不匹配 → 删除 .part，报错，状态 FAILED
4. rename .part → <tag>.tar.gz
```

**下载目录** `~/.edgeclaw/updates/`：故意**不**在 `rw_paths` 里（只有 `.edgeclaw/workspace` 是 rw），所以**模型和 shell 工具写不进去**，staged 二进制不会被篡改。这一点值得写进注释，因为它依赖的是已有的沙箱布局，不是新加的规则。

## 7. 解包

最小 tar 读取：只扫描 header，找到归档里**唯一**的 `*/edgeclaw`（Windows 为 `*/edgeclaw.exe`），流式 gunzip 出该条目。**其余条目全部跳过**（licenses 等与替换无关）。

校验点（任一条失败即中止）：

1. 归档里恰好一个匹配的文件（0 个 → "archive has no binary"；多个 → 拒绝，不猜）
2. 解出的文件大小在合理区间（例如 1 MB – 256 MB）
3. 写入 `~/.edgeclaw/updates/<tag>.new`，`chmod 0755`，`fsync`

## 8. 自替换

### 8.1 找到自己

新增 `edc_platform_exe_path()`：

| 平台 | 手段 |
|---|---|
| Linux | `readlink("/proc/self/exe")` |
| macOS | `_NSGetExecutablePath()` + `realpath` |
| Windows | `GetModuleFileNameW(NULL, ...)` |

### 8.2 前置检查（全部通过才允许替换）

| 检查 | 不通过时 |
|---|---|
| 目录可写（`access(W_OK)`） | "安装在 `<dir>`，当前用户无写权限；请用 `<install.sh>` 或包管理器更新" |
| 目录不是 bazel 输出（路径含 `/bazel-`） | "开发构建，不自我更新" |
| 版本不是 dirty/dev | 同上 |
| 拿到 install 锁 | "另一个更新正在进行" |

**install 锁**：`<exe_dir>/.edgeclaw-update.lock`，`O_CREAT|O_EXCL`。允许两个会话共用同一二进制时只有一个更新者。退出时删除（stale 由 mtime + 内容（pid）判定）。

### 8.3 替换原语

**POSIX（Linux/macOS）**

```
write   <dir>/.edgeclaw.new.$$     （已 fsync）
chmod   0755
rename  <dir>/.edgeclaw.new.$$  →  <exe>
fsync   <dir>
```

`rename()` 同文件系统内原子：读者要么看到旧 inode，要么看到新 inode。**正在运行的进程不受影响**（它持有旧 inode），所以替换后必须重启才生效。

**Windows**

运行中的 `.exe` 不能删除、不能覆盖。标准做法：

```
1. 备份：rename <exe> → <exe>.old.<pid>     （运行中的 exe 允许改名）
2. 放置：rename <new> → <exe>
3. 旧文件删除延后到下次启动（启动时扫 <exe>.old.* 并删）
```

如果第 2 步失败（极小窗口），**立刻把 `.old.<pid>` 改回原名**，保证程序还在。

### 8.4 替换前的最后一道验证

**在替换之前，先执行新二进制 `--version`，确认它报告的就是目标 tag。** 这一条同时排除：拿错平台、归档损坏、无法在本机执行（例如 glibc/musl 不匹配）。

执行时带 5 秒超时，不继承 TUI 的终端（否则会抢 stdin/termios）。

这一条是整个流程里性价比最高的检查，比任何静态校验都直接。

### 8.5 回滚的边界（重要）

替换成功后**不保证可以退回旧版本**：

- 旧二进制会被保留为 `<exe>.old.<pid>`（Windows）或直到下次启动（POSIX 没有旧副本——`rename` 覆盖了）
- 但**新版本一旦启动，数据库会向前迁移**（`EDC_DB_VERSION` 单向前进）。迁到 v9 之后，旧二进制**拒绝打开**这个库。

所以 UI 必须说清楚：**这个更新不会自动回退，更新后就回不去了**。旧文件的意义只是"替换过程被打断时能恢复"，不是降级手段。

## 9. UI

现有 `Toast` 是 1.4 秒自动消失的 chip，**不够**——C 档需要一次明确的确认。复用 `SetupDialog` 的模态页面模式。

| 阶段 | 呈现 |
|---|---|
| 发现新版本 | 顶部横幅（非模态）：`v1.0.1 可用 · /update 查看`，不打断输入 |
| 确认框 | 模态：版本号、release notes 链接、**"数据库会向前迁移，之后无法降级"**、`[更新] [取消]` |
| 下载/校验/替换 | 底栏进度：`下载中 42%` → `校验中` → `替换中`，`Esc` 可中止（`REPLACING` 之后不可） |
| 成功 | 模态：`已更新到 v1.0.1 · 重启后生效`，`[关闭]`。**不提供"立即重启"**——由用户自己决定何时重启（可能有任务在跑）。模态里给一句：`退出后重新启动 edgeclaw 即可` |
| 失败 | 模态：原因 + `[重试] [关闭]`，并给出手动更新命令 |

**线程**：检查/下载在 app 的 uv loop 上异步进行，结果通过 `IUiDispatcher::Post` 回到 UI 线程。绝不阻塞渲染。

## 10. 配置与持久化

```json
{
  "update_check": true,          // 新增，默认 true；false = 完全不联网检查
  "update_channel": "stable"     // 新增，默认 "stable"；"pre" 才收 prerelease
}
```

- `update_check=false` 时服务不注册，零网络。
- 与 `services_enabled` 的关系：`services_enabled=false` 会关掉**所有**后台服务（包括更新检查）。两者是"总开关 / 单项开关"。
- **首次运行必须告知**：`onboarded` 流程里加一句"EdgeClaw 会定期检查更新（可在 config.json 关掉）"。静默联网检查一个"处理私人对话"的工具是应该说明的。

## 11. 失败模式矩阵

| 场景 | 行为 |
|---|---|
| 离线 / DNS 失败 | 静默（只 log_debug），下次 tick 再说 |
| 403 / 429 限流 | 退避 24h，log_warn 一次 |
| 无匹配本平台资产 | 报告"该发布不含 <平台> 资产"，不重试 |
| sha256 不匹配 | 删除下载物，报错，**不重试**（可能是上游问题） |
| 磁盘不足 | 报错并给出所需空间 |
| 归档里没有/有多个二进制 | 拒绝，不猜 |
| 新二进制 `--version` 不符 | 拒绝替换，保留旧二进制 |
| 安装目录不可写 | 提示手动更新路径 |
| 替换中崩溃（Windows） | 下次启动扫 `.old.*` 恢复或清理 |
| 替换后启动，DB 迁移失败 | 已有备份机制（`edc_state_migrate`），报错并指向备份 |

## 12. 安全边界：明确不做的事

1. **不降级**。DB 单向迁移，降级必然失败。
2. **不在 dirty/dev 构建上更新自己**。
3. **不跳过校验和**。若 `sha256sums.txt` 缺失或无法解析 → 拒绝更新并提示手动更新。（`install.sh` 注释里的原则："an install script that silently skips verification once a checksum exists is worse than one that never claimed to"）
4. **不静默替换**。必须有确认。
5. **不在无写权限处尝试**提权。
6. **不把 API token 或任何用户数据发给更新端点**。请求里只有 `User-Agent`。
7. **不因为更新检查失败而影响启动**。

## 13. 测试计划

| 层 | 内容 |
|---|---|
| `edc_semver` | 纯函数：`1.0.0 < 1.0.1`、`1.0.0-rc.1 < 1.0.0`、`rc.2 > rc.1`、非法输入 |
| `edc_targz` | 用构造的归档：单文件、多文件、路径穿越（`../`）、截断、非 gzip |
| `edc_update` | 用本地 fixture 服务器（`test_https_client_get` 已有模式）：200/403/404、ETag 304、校验和不符、超大包 |
| `edc_selfreplace` | 在临时目录里放一个假"自身"，验证原子替换、并发锁、不可写目录拒绝、Windows 的 `.old` 恢复 |
| 端到端 | 构造一个假 release（本地 HTTP），把 `--version` 报告的目标版本做成一个真能跑的脚本，走完 检查→下载→校验→替换 |

**特别地**：`edc_selfreplace` 的测试绝不能替换真的测试二进制，必须用一个替身文件。

## 14. 分阶段落地

| 阶段 | 内容 | 可独立发布 |
|---|---|---|
| **0** | `sha256sums.txt` + `sha256sums.txt.sig` 产物 + `install.sh` 校验收紧 | ✅ 立刻可发，且是后续前置 |
| **1** | `edc_semver` + `edc_update`（检查 + 下载 + 双校验）+ 启动时一次性检查 + 横幅（**不替换**） | ✅ 纯增益，零风险 |
| **2** | `edc_targz` + `edc_selfreplace`（POSIX + Windows）+ 确认框 + 一键更新 | ✅ C 档本体 |
| **3** | 文档、`limitations.md`、密钥轮换说明 | ✅ |

阶段 0 单独一个 PR：它同时提升了现有 `install.sh` 的安全性，完全不碰自更新逻辑。

## 15. 已定与仍待确认

**已定**（§0）：一次启动一次检查、必须签名、默认开、不自动重启、Windows 支持。

**仍待你提供**：

1. **签名密钥对**。我给出 keygen 与转换脚本，私钥进 CI secrets（`UPDATE_SIGNING_KEY`），
   公钥提交进仓库。在此之前验签恒定失败，更新被拒绝（安全默认值）。
2. **Windows 首版的实际验证**。我可以写代码并让它在 CI 编译，但没有 Windows 实机可测；
   `.old` 清理路径只能在 CI 的 Windows job 里做"编译 + 单元测试"，实机行为需要你或有机器的人验证。
   这一条我会写进 `limitations.md` 的 Not-verified 表。

# TUI 聊天记录窗口化（对象层）方案 — 历史设计稿

> **实际交付状态（2026-11-20 更正，读下文前先看这里）**
>
> 本文是一份**设计提案**，下面的 §2（含 §2.4「高度几何」）与 §6 描述的是**最初设想的方案**，
> 不是最终落地实现。原文曾在此处声称「P0–P5 全部完成、目标已达成」，那是**不准确的**，理由：
>
> - §2.4 设想的「窗口外用 `est_lines` 估计高度 + 只测窗口内 + 有界 catch-up」**没有落地**。
>   `ChatBubbleRef` 的 `est_lines` / `measured_h` / `measured_w` 至今只被赋值、无人读取，
>   是死字段；`MessageList::EnsureMeasured()` 的首帧全量测量循环仍在。
> - 真正解决「滚动后缓存树累积、RSS 随历史增长」的是另一条路径（commit `f1c5bbc` + `63fbbf8`）：
>   **完全不动 `heights_` / `prefix_`，只让任意时刻同时存活的 ftxui 渲染树有界**
>   （帧边界清缓存 + 首帧测完后立刻清窗口外 proxy 树）。实测 DHAT 峰值 369 MB → 82 MB、
>   RSS ≈ 379 MB → ~93 MB。估计值方案反复撞 `prefix_`/滚动锚定的几何契约而未被采用。
> - 再之后 `chat_bubbles` 迁移（commit `466ff07`、`124103f`）把聊天记录改成 **DB 权威 + 分页**，
>   运行时 `chat.jsonl` 已彻底退役（commit `74bb9b2`）；本文 §2 里以 transcript 文件为视图权威、
>   `kFile | kMemory` 双源、`ITranscriptStore::pread` 等描述也**不再是当前架构**。
>
> 因此：下文作为**历史设计记录**保留（它解释了为什么不走估计高度路线、以及各种几何约束），
> 但**不要**把其中的"完成/目标达成"字样或 §2.4 当作现状；当前实现以源码、m107/m109 的交付记录
> 和 `docs/chat-bubbles-in-db.md` 为准。

**第二批提交（P2–P5，最终形态）**
- 删掉 progressive backfill（线程 + `backfill_*` 成员 + `ReplayTail` + "Loading earlier history" 提示）与
  `PrependMessages` / `Change::prepended` / `TopIsBackfilled()`；`TUIWindow::LoadChatTranscriptFile`
  改为一次性 `ChatModel::LoadTranscript(path)`。
- `ChatBubbleRef` 支持 `kFile | kMemory` 双源：历史从 transcript 按 offset 解码；本次进程产生的气泡
  （含界面反馈）只存内存快照，**不写文件**，滚走即释放、滚回可重建。
- `ChatModel::all_/visible_` 改为 `LazyBubble` 轻量代理（无正文、无渲染树），真实 `Message`
  只在 `MessageWindow::live_`（≤ 200）；`MessageList` 渲染/几何/锚定代码几乎未动。
- 展开态与实测高度存在 descriptor 上（P4），窗口滑动不丢。
- `test_chat_window` 去掉 `manual`：**常驻有界断言转绿**，并新增「滚到头/尾帧不空白」回归测试
  （已验证在旧渲染委托方式下真的红）。
- `test_message_list` 的 3 个 prepend 断言迁到 transcript 驱动；`CountingMessage` 计数断言改为
  直接断言 `LiveCount() <= 200`（窗口化后语义更强）。`test_scroll_regression` 删掉已不存在的
  backfill-hint 门控测试。

**根因记录（滚动端点帧空白）**：`LazyBubble` 最初通过 `real->Render()`（ComponentBase 包装）或
返回临时 Element 委托给游离的真实气泡；ftxui 的测量阶段与绘制阶段必须看到同一个缓存 Node，
否则滚到头/尾后整帧留白（几何 `total_/prefix_/可见区间/hits_` 全部正确）。最终修法：由
正确子类的 `real->OnRender()` 生成树，**缓存在 proxy 自己的 `body_` 上**，`SetRenderWidth` 变更时
失效；点击 toggle 后清空缓存，展开态写回 descriptor。

**验证**：`bazel test //tests:all` 154/154；`bazel build //tests:all`；`gen_notices.py --check`
19 files；`check_portability.py` 271 files。

**进度快照（第一批提交，已被第二批取代）**
- P0：`tests/test_chat_window` 测「常驻实例数必须有界」，在旧实现上实测
  `live=1000 > cap 200`，失败信息精确指向 O(history)；带防空洞守卫。第一批按项目约定标
  `manual` 避免染红全量，**P2 完成后已去掉该 tag 并转绿**。
- P1 数据层：`src/tui/edc_chat_window.{hpp,cpp}` 提供 `ChatBubbleRef` 索引（单次顺序扫、
  offset/length 精确寻址、跨 256 KiB chunk 正确）、`MessageWindow`（`kMaxLive=200`、滑动淘汰、
  状态在 ref 上）。`tests/test_chat_refs` 6/6 通过。
- 共享工厂：`MakeChatBubble()` 提到 `edc_message.cpp`，各路径共用。
- 第二批见文首「实际交付状态」更正说明（原「P0–P5 全部完成」的说法已被更正，勿当作现状）。

**B 方案遗留的设计缺口（P2 开工前必须补设计）**：`MessageWindow::Get(i)` 从 transcript 行
解码，但 B 方案明确「界面反馈气泡（task N finished / interrupted hint）不落盘」，
它们没有对应的 transcript 行。要同时满足「不落盘 + 滚走即释放 + 滚回还在」，需要一个
**内存行缓冲层**（in-process bubbles 的 refs 指向内存而非文件 offset）。当前 `ChatBubbleRef`
只有文件 offset，需在 P2 前扩展成「文件行 | 内存行」的联合寻址。这是评审时未覆盖的一点。

**评审决定（用户确认）**：`kMaxLive = 200` 合适；接受窗口外用估计高度带来的滚动条轻微修正；
删除 `PrependMessages` 兼容接口；按 P0–P5 全部实施。

**关于「transcript 作视图权威」（评审问题 1 的结论）**：DB 的 `messages` 表记的是模型状态
（发给/收自 LLM 的对话消息），transcript 文件记的是视图状态（屏幕上出现过的每一个气泡，
含 kind / render_pref / task_id / header / content）。两者刻意互不推导：
`edc_chat_transcript.hpp` 明写「Replaying session.json resurrects messages that were never
on screen」。只存在于 transcript 的包括 echoed user input（`edc_tui_window.cpp:440`）、
命令回复与 error、`sys` 气泡、后台 session 在切走时的输出（`:1986-1992`，只写它自己的
transcript、当前 tab 不显示）、以及渲染偏好；只存在于 DB 的包括 system prompt、turn 结构、
fold 裁掉的消息。此外「task N finished」「interrupted hint」这类**界面反馈**两者都不写
（`:1898`/`:1928` 注释「deliberately NOT written -- this is interface feedback」），它们本就
不该在重启后复现。因此窗口化后：历史气泡从 transcript 按视口 `pread` 解码，本次进程新产生
的气泡（含界面反馈）留在 live 窗口、Append 过的进 transcript，滚走即释放 —— 权威不变，
只把「什么时候读」从启动全量改成按需。

---

## 0. 现状的精确诊断（review 结论修正）

上一轮说「TUI 没有窗口化」不准确。`src/tui/edc_message_list.hpp` 的注释和
`MessageListNode::Render()` 表明：**绘制层已经窗口化** —— 只 `Render()` 与 stencil 相交的
item，用 `prefix_[]` 行映射 + stencil 裁剪，`heights_`/`stale_`/`prefix_` 提供滚动几何，
`IScrollAnchor` 保证重测高度时视口不跳动。

真正全量常驻的是**对象与缓存**，共 4 份，各自 O(历史条数)：

| # | 位置 | 内容 | 为何全量 |
|---|---|---|---|
| 1 | `MessageList::items_` | `vector<shared_ptr<Message>>` | 全量持有 |
| 2 | `ChatModel::all_` | 同一批 `shared_ptr` 的第二份 vector | 「The log keeps everything; the filter only decides what is drawn」 |
| 3 | `Message::body_` / `md_body_` / `preview_` | 每条的 ftxui 渲染树 + 预览副本 | `edc_message.cpp:111-123` 首次渲染即缓存，注释明说 "even when scrolled out of view"，只有 `OnWidthChanged()` 才丢 |
| 4 | `MessageList::heights_` / `stale_` / `prefix_` | 每条的高度/几何数组 | 滚动几何需要 `total_`，全量数组是设计前提 |

**触发点**：`MessageList::EnsureMeasured()` 的 `edc_message_list.cpp:404-411` 会遍历
`for (i = 0; i < items_.size(); i++) if (heights_[i] == kUnmeasured) MeasureAt(i, width);`，
而 `MeasureItem()` 调 `items_[i]->Render()` → 触发 `Message::OnRender()` 建树并缓存。
即**首帧就会为整份 transcript 的每一条建立渲染树**。

另外 `TUIWindow::LoadChatTranscriptFile()` 先 `ReplayTail(200)`，再
`StartTranscriptBackfill` 把 `[0, prefix_end)` 的**全部**老历史分批 `PrependMessages`
灌进 `all_` / `items_`。分批（100 条 / 64 KiB 每帧）只是防 UI 卡顿，不是内存上限。

其余（kernel 侧 `group->history` id slot、`group->memory`、session memo detail、
`task->output`）量级小或已有上界，**本次不动**。

---

## 1. 设计原则

**文件是唯一权威，内存是视口的投影。** 与 kernel 侧已有的两条原则对齐：

- `edc_task_group.c:197`：「a history that costs a pointer per message and one that costs
  the message」
- `edc_session_store.c:250`：「The in-memory tree is a CACHE of the database, not the
  source of truth」

TUI 的权威是**chat transcript 文件**（`edc_chat_transcript.hpp` 明确：它是 *view state*，
DB 是 *model state*；echoed user input / 命令回复 / error / finish notice / 明写
`deliberately NOT written` 的界面反馈气泡只在 transcript 里）。**不合并这两层、不改成从 DB
读**，否则会丢视图语义并引入兼容层。

---

## 2. 数据结构

### 2.1 新增 `ChatBubbleRef`（轻量索引项，替代 `all_` 里的 `shared_ptr<Message>`）

```
struct ChatBubbleRef {
    uint64_t  offset;        // transcript 中该行的起始字节
    uint32_t  length;        // 行字节数（含 '\n'）
    int8_t    kind;          // user|tool|thought|reply|sys
    int8_t    render_pref;   // EDC_APP_RENDER_PLAIN / _MARKDOWN
    uint32_t  task_id_off;   // 行内字段偏移（避免重复存字符串）
    uint32_t  task_id_len;
    uint32_t  header_len;    // 供估计高度用
    uint32_t  content_len;   // 供估计高度用
    uint16_t  est_lines;     // 折叠态估计行数（不建树即可得）
    bool      expanded;      // ★ 原 Message::expanded_ / thought_expanded_ 上提
    int16_t   measured_h;    // -1 = 未测；否则该宽度下已测行数
    uint32_t  measured_w;    // 该高度对应的宽度（宽度变了即失效）
};
```

预计 ~48 字节/条（对齐后），40k 条约 2 MB，且**不含任何正文与渲染树**。

### 2.2 `MessageWindow`（新增，放在 `ChatModel` 与 `MessageList` 之间）

- `vector<ChatBubbleRef> refs_` —— 全量索引，唯一长期状态
- `ITranscriptStore *store_` + `path_` —— 按需 `pread(offset, length)` + `DecodeTranscriptLine`
- `unordered_map<size_t, shared_ptr<Message>> live_` —— **仅视口±预取窗口内的实例**
- `Window(first, last)` —— 由 `MessageList` 的 `visible_first_/visible_last_` 驱动：
  - 进入窗口的 index：解码 → `MakeChatWidget(...)` → 应用 `refs_[i].expanded` → 放入 `live_`
  - 离开窗口的 index：`live_.erase(i)` → 对象析构 → `body_`/`md_body_`/`preview_` 一并释放
- 窗口大小：`kPrefetch = 2 * 视口条数`（上下各一屏），上限 `kMaxLive = 200`，避免宽屏/超小
  高度下失控

### 2.3 常驻 `Message` 的处理

**新追加的气泡**（`AddMessage`）在进程内产生，本就没有文件行：为它单独保留一个
`live_` 条目 + 立即 `Append` 到 transcript 并补一条 `ChatBubbleRef`。这样「新气泡」与
「历史气泡」走同一条 `live_` 路径，不引入第二套机制。

### 2.4 高度几何（关键，不能退化）

> ⚠️ **此方案未被采纳**：下面的「窗口外估计高度 + 只测窗口内」从未落地（`est_lines` /
> `measured_h` / `measured_w` 是死字段）。实际交付见文首更正说明与 commit `63fbbf8`。
> 本段保留是为了记录几何约束，说明后来为什么选择"只让同时存活的树有界"这条路。

`MessageList` 的 `ComputeRequirement()` 需要 `total_`（全内容高度）才能算滚动范围。
保留 `heights_`/`stale_`/`prefix_` **全量 int 数组**（4 字节/条，40k 条 = 160 KB / 数组，
可接受），但语义改为：

- 窗口外的 `heights_[i]` 用 `refs_[i].est_lines + kSpacingRows` 作**估计值**
- `EnsureMeasured()` 删掉 404-411 那个全量测量循环，改为「只测窗口内 + 有界 catch-up」
  （现有 `kCatchUpItems = 64` 的机制保留，只是不再首帧扫全量）
- 窗口内实测后写回 `refs_[i].measured_h/measured_w`，`prefix_`/`total_` 通过既有
  `Rebuild()` + `IScrollAnchor::TakeOffsetAdjustment()` 修正 —— **这套锚定机制原样复用**，
  正是它让「估计值被实测替换」对滚动位置不可见

> 这是本方案最依赖现有设计的部分：估计高度 + 滚动锚定已经存在且在 `edc_message_list.hpp`
> 注释里写明，我们只是不再为全量建立精确值。

---

## 3. 改动点清单

| 文件 | 改动 |
|---|---|
| `src/tui/edc_chat_bubble_ref.hpp`（新） | `ChatBubbleRef` + `BuildRefs(path)` 单次顺序扫描（只切 4 个 tab、记 content 字节范围，不保留正文） |
| `src/tui/edc_message_window.hpp/.cpp`（新） | `MessageWindow`：refs 索引 + `live_` 窗口 + `Load(i)` / `SetWindow(first,last)` / `ReloadExpanded(i)` |
| `src/tui/edc_chat_transcript.hpp/.cpp` | 新增 `DecodeLineAt(path, offset, length, out)`（复用现有 `DecodeTranscriptLine`，不重复实现转义规则）；`LoadTranscriptTail` 已给出精确 `*prefix_end` 字节偏移，可直接用于建索引 |
| `src/tui/edc_chat_model.hpp/.cpp` | `all_` / `visible_` 从 `vector<shared_ptr<Message>>` 改为「`refs_` 索引 + 过滤后的 index 序列」；`Rebuild()` 只重算 index 序列；`Change` 通知改为携带 index 范围而非 `shared_ptr`（`appended`/`prepended` 语义保留） |
| `src/tui/edc_message_list.hpp/.cpp` | `items_` 改为 `MessageWindow*` 或 `vector<weak_ptr<Message>>` 视图；`MeasureItem()` 改为 `window->Load(i)`；删全量测量循环；渲染时 `window->Get(i)` |
| `src/tui/edc_chat_widget.cpp` | `OnModelChanged` 适配新的 `Change`（append/prepend/rebuild → 更新 refs 序列 + 调 `SetWindow`） |
| `src/tui/edc_tui_window.cpp` | 删 `StartTranscriptBackfill` / `PumpTranscriptBackfill` / `CancelTranscriptBackfill` 及 `backfill_*` 成员；`LoadChatTranscriptFile()` 改为「建 refs 索引 → 定位到尾部窗口」；追加气泡时同步 Append + 补 ref |
| `src/tui/edc_message.hpp/.cpp` | `expanded_` / `thought_expanded_` 状态上提到 `ChatBubbleRef`（`Message` 变成纯视图，可随时重建） |
| `src/tui/edc_transcript_store.hpp` | `ITranscriptStore` 可能加一个 `ReadLineAt` 虚方法（或让 `MessageWindow` 直接持有 path + `ifstream`，倾向后者，接口不变） |
| `tests/BUILD` | 新增 `test_chat_window` 靶 |

**明确不动**：kernel 侧 group/task/turn/history、DB schema、`flush`/`delete_stale`、
session memo、transcript 文件格式（保持 append-only、格式冻结）。

---

## 4. 分阶段实施（每阶段独立可验证、可回滚）

**P0 — 度量基线（不改行为）**
在 `MessageWindow` 里加一个调试计数 `live_count()`，写 `test_chat_window` 断言当前实现下
`live_count()` 随 ref 条数线性增长（先看到"红"的那一面：先写一个期望 O(视口) 的断言，
对旧实现故意失败，确认它真的测到了问题，再进入 P1）。按会话约定，这一步必须真的见到红灯。

**P1 — 索引 + 尾部窗口**
建 `BuildRefs`，`LoadChatTranscriptFile` 只加载尾部 200 条到 `live_`；删 backfill 线程。
验证：启动后 RSS 与历史长度解耦；`live_count() <= kMaxLive`。

**P2 — 滚动驱动窗口**
`MessageList::Render()` 算出 `first/last` 后调 `MessageWindow::SetWindow()`；离开窗口即析构。
验证：向上滚到任意位置、再滚回，内容一致、`live_count()` 始终有界。

**P3 — 高度估计与锚定**
删全量测量循环，窗口外用估计值，复用 `IScrollAnchor` 修正。验证：滚动条高度/位置在
展开 ToolMessage、改宽度、PageUp 后不跳。

**P4 — 状态上提与过滤**
`expanded_` 上提；`ChatModel` 改为 index 序列；`SetFilter`/`SetTaskInputs` 回归。
验证：过滤切换后展开态保持、task input 前置行正确。

**P5 — 清理 + 全量回归**
删 `PrependMessages`/`Change::prepended` 等不再需要的路径；跑 `bazel test //tests:all`、
`bazel build //tests:all`、两个 portability 脚本、`gen_notices.py --check`。

---

## 5. 回归断言（可写死的性质）

1. **常驻有界**：构造 40,000 条 transcript → `live_count() <= kMaxLive`，且
   `BuildRefs` 后 `refs_.size() == 40000`，`peak_rss` 相对 200 条基线增长 < 阈值。
2. **内容等价**：对随机 index 集合，`Load(i)` 得到的 `Message` 与旧
   `Replay()` 全解码得到的同 index 消息，`kind/task_id/header/content` 逐字段相等。
3. **窗口滑动**：`SetWindow(0,50)` → `SetWindow(39950,40000)` → 再回 `0..50`，
   每次 `live_` 的 key 集合 ⊆ 请求区间±预取；旧对象被析构（`weak_ptr::expired()` 断言）。
4. **展开态持久**：展开第 i 条 → 滚出窗口 → 滚回 → 仍展开（状态在 ref 上）。
5. **滚动锚定**：窗口外估计高度 → 滚入实测 → `TakeOffsetAdjustment()` 后视口顶部仍是
   同一条（复用 `test_message_list.cpp` 的锚定断言）。
6. **过滤语义**：`test_chat_filter.cpp` 现有断言全部保持通过；`SetFilter` 后 index 序列
   与旧可见序列逐条对应。
7. **追加一致**：运行中 `AddMessage` 后，transcript 文件、refs 索引、`live_` 三者同条数，
   重启后该气泡仍可回放。

> 按会话约定 P0 要先让 #1 在旧实现上**真的红**，否则不算回归测试。

---

## 6. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 估计高度不准 → 滚动条跳动/滚动位置漂 | 可读性 | 复用已存在的估计 + `IScrollAnchor` 机制；`est_lines` 按 kind 给保守值（ToolMessage 折叠态 = `min(14, lines)+chrome`）；P3 单独验证 |
| 每次滚动 `pread` + decode + 建树 | 滚动掉帧 | 上下各一屏预取；`live_` 命中则不重建；`DecodeLineAt` 只读一行；建树成本与当前"首帧全量建树"相比是净减少 |
| `Message` 内 UI 状态（展开/选中/高亮）丢失 | 交互回退 | P4 把状态上提到 `ChatBubbleRef`；`highlighted_` 是每帧临时量，随对象析构无影响 |
| transcript 文件被外部修改 / 半行 | 解码失败 | 现有 `DecodeTranscriptLine` 已处理不可解码行与尾部残行；`BuildRefs` 只在建索引时扫一次，运行期 `Append` 自己维护 offset，不重扫 |
| 过滤下 index 序列与 `MessageList` 的 `items_` 下标错位 | 显示错乱 | `ChatModel::visible_` 明确定义为「refs 下标序列」，`MessageList` 只按这个序列的下标访问 `MessageWindow`；P4 用现有 filter 测试守住 |
| 改动面横跨 model/list/widget/window | 回归风险大 | 分 P0–P5 六阶段，每阶段跑 `test_chat_*` + `test_message_list`；不改 kernel、不改 DB、不改文件格式 |

---

## 7. 与「group 卸载」方案的关系（供对照）

本方案**不实现** group 空闲卸载。理由：group 结构里已无消息正文（只有 id slot），卸载
释放的是 KB 级元数据；且 kernel `flush` 的 `delete_stale` 会删除「DB 有、内存树没有」的
行，需改造全量比对语义，是唯一可能真丢数据的改法。若后续仍需降低 session memo detail
（200 条上界、MB 级），可作为独立的小改动，与本方案无耦合。

---

## 8. 评审结论（已定）

| # | 议题 | 决定 |
|---|---|---|
| 1 | 权威层 | 确认 transcript 文件为视图层权威、不从 DB 读（见开头「评审决定」一节的展开） |
| 2 | 窗口策略 | `kPrefetch = 1 屏`、`kMaxLive = 200`；不引入「滚动停止后才回收」宽限期（先按简单规则实现，若实测快速滚动掉帧再议） |
| 3 | 估计高度 | 接受滚入老历史时滚动条的轻微修正，换来不建全量渲染树；不做启动一次性全量测量 |
| 4 | 兼容接口 | 删除 `PrependMessages` 及 `Change::prepended`（目前只被 backfill 使用） |
| 5 | 阶段切分 | P0–P5 全部实施，逐阶段验证 |

# GLib → libuv + 标准C 渐进迁移方案

## 当前状态

- **37 个文件**直接 `#include <glib.h>`（32 个 `src/` + 5 个 `tests/`）
- **GLib 版本**: 2.82.2 (BCR)，有 2 个 ARM 兼容性补丁
- **构建系统**: 纯 Bazel
- **目标平台**: ARM/ARM64 Linux, ARM/ARM64 Android, ARM64 macOS, amd64 Linux
- **libuv**: 项目中尚未引用

### GLib 使用分布

| 类别 | 复杂度 | 影响文件数 | 替换方案 |
|------|--------|-----------|----------|
| 基础类型别名 (gchar, gboolean, gint 等) | 低 | ~30 | 标准C (`<stdbool.h>`, `<stdint.h>`) |
| 内存管理 (g_new, g_new0, g_malloc0, g_free) | 低 | ~30 | 标准C (`malloc`, `calloc`, `free`) |
| 字符串工具 (g_strdup, g_strsplit 等) | 中 | ~15 | 标准C / 自定义 |
| GString 动态字符串 | 中 | ~25 | **SDS**（项目已有依赖） |
| 数据结构 (HashTable, Array, List, Queue) | 中高 | ~15 | 自定义实现 |
| 线程与同步 (GThread, GMutex, GAsyncQueue) | 中 | ~6 | **libuv** |
| 事件循环 / IO 监听 (GMainLoop, GSource) | **高** | ~8 | **libuv** |
| 文件系统操作 (g_file_*, g_dir_*, g_build_filename) | 低 | ~10 | POSIX |
| 进程管理 (g_spawn_*) | 中 | ~2 | **libuv** |
| 杂项 (UUID, Base64, UTF8, 日期, Regex, usleep) | 低 | ~5 | 标准C + mbedtls |

### 不会使用的 GLib 子系统

以下 GLib 子系统在项目中没有使用，迁移时无需关注：
GObject, GSignal, GVariant, GKeyFile, GChecksum, GRand, GByteArray, GPtrArray, GBytes, GSocket

---

## Phase 0: 准备工作

**预估**: 1-2 天 | **风险**: 无

### 0.1 添加 libuv 依赖

在 `MODULE.bazel` 中添加：
```python
bazel_dep(name = "libuv", version = "1.48.0")
```

### 0.2 创建兼容层头文件 `src/edc_compat.h`

提供过渡类型宏，让代码可逐步迁移：
```c
#ifndef EDC_COMPAT_H
#define EDC_COMPAT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 类型别名 */
#define gchar    char
#define gboolean bool
#define gint     int
#define guint    unsigned int
#define gint64   int64_t
#define gint32   int32_t
#define gpointer void*
#define gsize    size_t
#define gssize   ssize_t
#define guchar   unsigned char

/* 内存宏 */
#define g_new(type, n)     ((type*)malloc((n) * sizeof(type)))
#define g_new0(type, n)    ((type*)calloc((n), sizeof(type)))
#define g_malloc0(n)       calloc(1, (n))
#define g_free             free

#endif /* EDC_COMPAT_H */
```

### 0.3 将 SDS 加入 agent 依赖

SDS 已在项目中（`@sds//:sds`），当前仅 `device_agent_lib` 使用。需要在 `src/BUILD` 的 `agent` target 的 `deps` 中添加 `@sds//:sds`。

### 0.4 验证构建

```bash
bazel build //src:agent //src:edgeclaw //src:device_agent
```

---

## Phase 1: 类型别名 + 内存管理替换

**预估**: 3-5 天 | **风险**: 极低（纯机械替换）

### 1.1 批量替换

对所有 `.c` 和 `.h` 文件执行：

| 查找 | 替换为 |
|------|--------|
| `gchar` | `char` |
| `gboolean` | `bool` |
| `gint` (非 gint64/gint32) | `int` |
| `guint` | `unsigned int` |
| `gint64` | `int64_t` |
| `gint32` | `int32_t` |
| `gpointer` | `void*` |
| `gconstpointer` | `const void*` |
| `gsize` | `size_t` |
| `gssize` | `ssize_t` |
| `guchar` | `unsigned char` |
| `g_free(...)` | `free(...)` |

### 1.2 宏替换

| 查找 | 替换为 |
|------|--------|
| `g_new(type, n)` | `EDC_NEW(type, n)` |
| `g_new0(type, n)` | `EDC_NEW0(type, n)` |
| `g_malloc0(n)` | `EDC_MALLOC0(n)` |

其中 `EDC_NEW` / `EDC_NEW0` / `EDC_MALLOC0` 在 `edc_compat.h` 中定义。

### 1.3 头文件清洗

对仅使用了类型别名 + 内存管理（无其他 GLib API）的文件，将 `#include <glib.h>` 替换为：
```c
#include "edc_compat.h"
```

### 1.4 验证

```bash
bazel build //src/... //tests/...
bazel test //tests/...
```

---

## Phase 2: 字符串工具函数替换

**预估**: 3-5 天 | **风险**: 低（需正确处理 NULL 语义差异）

### 2.1 创建 `src/edc_strutil.h` / `src/edc_strutil.c`

实现以下函数：

| GLib API | 替换实现 | 说明 |
|----------|---------|------|
| `g_strdup(s)` | `strdup(s)` | POSIX，直接可用 |
| `g_strdup_printf(fmt, ...)` | `edc_strdup_printf(fmt, ...)` | 基于 `vasprintf` 或 `snprintf` |
| `g_strcmp0(a, b)` | `edc_strcmp0(a, b)` | NULL 安全的 strcmp：NULL == NULL 返回 0，NULL < 非NULL 返回 -1 |
| `g_str_equal(a, b)` | `edc_str_equal(a, b)` | 用于哈希表，等价 `edc_strcmp0(a, b) == 0` |
| `g_str_hash(s)` | `edc_str_hash(s)` | djb2 或 FNV-1a 哈希 |
| `g_direct_hash(p)` | `edc_direct_hash(p)` | 指针值作为哈希 |
| `g_direct_equal(a, b)` | `edc_direct_equal(a, b)` | `a == b` |
| `g_str_has_prefix(s, prefix)` | `edc_str_has_prefix(s, prefix)` | `strncmp(s, prefix, strlen(prefix)) == 0` |
| `g_strstrip(s)` | `edc_strstrip(s)` | 原地去除前后空白，返回 s |
| `g_strv_length(strv)` | `edc_strv_length(strv)` | 计算 NULL-terminated 字符串数组长度 |
| `g_strfreev(strv)` | `edc_strfreev(strv)` | free 字符串数组及其内容 |
| `g_strsplit(s, sep, max)` | `edc_strsplit(s, sep, max)` | 按分隔符拆分，返回 NULL-terminated 数组 |
| `G_GINT64_FORMAT` | `PRId64` | `#include <inttypes.h>`，`"%lld"` → `"%" PRId64` |
| `GDestroyNotify` | `typedef void (*EdcDestroyNotify)(void* data);` | 类型别名 |
| `GINT_TO_POINTER(i)` | `((void*)(intptr_t)(i))` | 宏 |
| `G_GUINT64_FORMAT` | `PRIu64` | （如使用） |

### 2.2 批量替换

在所有文件中将对应 API 调用替换为新函数名。

**注意**: `g_str_equal` / `g_str_hash` 在哈希表初始化时作为函数指针传入，需同步替换。

### 2.3 特殊处理

`g_strfreev` 的调用模式通常是：
```c
g_strfreev(arr);  // arr 是 gchar**
```
替换为：
```c
edc_strfreev(arr);  // arr 是 char**
```

### 2.4 验证

```bash
bazel build //src/... //tests/...
bazel test //tests/...
```

---

## Phase 3: GString → SDS 替换

**预估**: 3-5 天 | **风险**: 中（影响面广，约 25 个文件）

### 3.1 SDS 已在项目中

SDS (Simple Dynamic Strings) 已在 `MODULE.bazel` 扩展中定义（`@sds//:sds`），当前仅 `device_agent_lib` 使用。

#### 在 `src/BUILD` 中将 SDS 加入 agent 依赖：
```python
cc_library(
    name = "agent",
    deps = [
        "@sds//:sds",  # 新增
        # ... 其他依赖
    ],
)
```

### 3.2 API 映射表

SDS 的 `sds` 类型实际是 `char*` typedef，直接可当 C 字符串使用。

| GString 操作 | SDS 等价操作 | 说明 |
|-------------|-------------|------|
| `GString *s;` | `sds s;` | sds 是 `char*` |
| `g_string_new(str)` | `sdsnew(str)` | 或用 `sdsempty()` 创建空串 |
| `g_string_sized_new(n)` | `sdsnewlen(NULL, n)` | 预分配 n 字节 |
| `g_string_new_take(str)` | `sdsnewlen(str, strlen(str))` + `free(str)` | SDS 不会接管内存，需手动复制后释放原指针 |
| `g_string_free(s, TRUE)` | `sdsfree(s)` | 释放内存 |
| `g_string_free(s, FALSE)` | 返回 `s` 本身（char* 可直接使用） | SDS 提供 `sdslen()` 获取长度 |
| `g_string_append(s, val)` | `s = sdscat(s, val)` | **注意**: SDS 修改后需重新赋值 |
| `g_string_append_len(s, val, len)` | `s = sdscatlen(s, val, len)` | **注意**: 需重新赋值 |
| `g_string_printf(s, fmt, ...)` | `s = sdscatprintf(s, fmt, ...)` | **注意**: 需重新赋值 |
| `g_string_assign(s, val)` | `s = sdscpy(s, val)` | **注意**: 需重新赋值 |
| `s->str` | 直接用 `s` | SDS 即 C 字符串 |
| `s->len` | `sdslen(s)` | 获取长度 |
| `g_string_truncate(s, n)` | `sdsrange(s, 0, n)` | 截断 |
| `g_string_insert_len(s, pos, val, len)` | `s = sdscatlen(sdsRemoveFreeSpace(s, pos), val, len)` | 较复杂，按需实现 |

### 3.3 迁移注意事项

1. **SDS 重新赋值问题**: SDS 操作可能触发 realloc 改变指针地址，所有 `s = sdscat*(s, ...)` 模式的调用必须将返回值重新赋给原变量。

2. **`g_string_free(s, FALSE)` 模式**: GString 有时用作"借出"内部 buffer。SDS 中：
   - 如果后续还要用 SDS 函数，继续持有 sds
   - 如果只需原始 char*，直接使用 s（然后 `free(s)` 而非 `sdsfree(s)`）

3. **结构体中的 GString 字段**: 替换为 `sds` 后，初始化和清理逻辑需对应调整。

4. **建议的迁移策略**: 创建薄封装宏/内联函数，保持代码改动最小：
   ```c
   #include "sds.h"
   
   typedef sds EdcString;
   #define edc_string_new(s)       sdsnew(s)
   #define edc_string_free(s)      sdsfree(s)
   #define edc_string_cat(s, v)    s = sdscat(s, v)
   #define edc_string_catlen(s, v, l) s = sdscatlen(s, v, l)
   #define edc_string_printf(s, f, ...) s = sdscatprintf(s, f, ##__VA_ARGS__)
   #define edc_string_len(s)       sdslen(s)
   ```

### 3.4 验证

```bash
bazel build //src/... //tests/...
bazel test //tests/...
```

---

## Phase 4: 数据结构替换

**预估**: 5-7 天 | **风险**: 中高

创建 `src/edc_containers.h` / `src/edc_containers.c`，实现以下数据结构。

### 4a. GHashTable → EdcHashTable

**使用位置**: `edc_agent.c`, `edc_channel_actor.c`, `edc_skills.c`, `edc_toolbox.c`

**设计**: 链式哈希表（chaining），支持自定义 hash/equal 函数和 destroy notifier。

```c
typedef size_t (*EdcHashFunc)(const void* key);
typedef bool   (*EdcEqualFunc)(const void* a, const void* b);
typedef void   (*EdcDestroyNotify)(void* data);

typedef struct EdcHashTable EdcHashTable;

EdcHashTable* edc_ht_new_full(EdcHashFunc hash_fn, EdcEqualFunc equal_fn,
                               EdcDestroyNotify key_destroy,
                               EdcDestroyNotify value_destroy);

void*  edc_ht_lookup(EdcHashTable* ht, const void* key);
bool   edc_ht_insert(EdcHashTable* ht, void* key, void* value);
bool   edc_ht_remove(EdcHashTable* ht, const void* key);
size_t edc_ht_size(EdcHashTable* ht);
void** edc_ht_get_keys(EdcHashTable* ht);   /* 返回 caller-free 的数组 */
void   edc_ht_destroy(EdcHashTable* ht);
```

### 4b. GArray → EdcArray

**使用位置**: `edc_actor.c`, `edc_llm_actor.c`, `edc_skills.c`, `edc_tools_fs.c`, `edc_agent.c`, `edc_memory_actor.c`, `edc_context.c`

**设计**: 动态数组，类似 C++ `std::vector`。

```c
typedef struct EdcArray {
    char* data;       /* 元素存储 */
    size_t len;       /* 当前元素数 */
    size_t capacity;  /* 已分配容量 */
    size_t elem_size; /* 每个元素大小 */
} EdcArray;

EdcArray* edc_array_new(size_t elem_size);
void      edc_array_append_vals(EdcArray* arr, const void* data, size_t n);
void*     edc_array_index(EdcArray* arr, size_t index);
void      edc_array_free(EdcArray* arr, bool free_elements);
```

**旧 API 映射**:
- `g_array_new(FALSE, FALSE, sizeof(T))` → `edc_array_new(sizeof(T))`
- `g_array_append_val(arr, val)` → `edc_array_append_vals(arr, &val, 1)`
- `g_array_index(arr, T, i)` → `((T*)edc_array_index(arr, i))`
- `g_array_free(arr, TRUE)` → `edc_array_free(arr, true)`
- `arr->len` → `arr->len`

### 4c. GSList → EdcSList

**使用位置**: 大量 tools 文件、channel 文件

**设计**: 单向链表。

```c
typedef struct EdcSList {
    void* data;
    struct EdcSList* next;
} EdcSList;

EdcSList* edc_slist_append(EdcSList* list, void* data);
EdcSList* edc_slist_prepend(EdcSList* list, void* data);
EdcSList* edc_slist_remove(EdcSList* list, const void* data);
void      edc_slist_free(EdcSList* list);
void      edc_slist_free_full(EdcSList* list, EdcDestroyNotify free_fn);

/* 迭代宏 */
#define EDC_SLIST_FOREACH(list, iter) \
    for (EdcSList* iter = (list); iter != NULL; iter = iter->next)
```

### 4d. GList → EdcList

**使用位置**: `edc_session.c`, `edc_toolbox.c`, `edc_agent.c`, `edc_channel_actor.c`

**设计**: 双向链表。

```c
typedef struct EdcList {
    void* data;
    struct EdcList* prev;
    struct EdcList* next;
} EdcList;

EdcList* edc_list_append(EdcList* list, void* data);
EdcList* edc_list_prepend(EdcList* list, void* data);
EdcList* edc_list_remove(EdcList* list, void* data);
void     edc_list_free(EdcList* list);
void     edc_list_free_full(EdcList* list, EdcDestroyNotify free_fn);
```

### 4e. GQueue → EdcQueue

**使用位置**: `edc_network_async.c`, `edc_turn.c`, `edc_session.c`, `edc_llm_actor.c`

**设计**: 基于 EdcList 实现，FIFO 语义。

```c
typedef struct EdcQueue {
    EdcList* head;
    EdcList* tail;
    size_t length;
} EdcQueue;

void   edc_queue_init(EdcQueue* q);
void   edc_queue_clear(EdcQueue* q);
void   edc_queue_push_tail(EdcQueue* q, void* data);
void*  edc_queue_pop_head(EdcQueue* q);
void*  edc_queue_peek_head(EdcQueue* q);
size_t edc_queue_length(EdcQueue* q);

typedef void (*EdcFunc)(void* data, void* user_data);
void   edc_queue_foreach(EdcQueue* q, EdcFunc func, void* user_data);
```

**注意**: 当 GQueue 被用作结构体字段（值类型而非指针）时，需改用它处分配 + 初始化模式：
```c
// 旧:
GQueue queue = G_QUEUE_INIT;
// 新:
EdcQueue queue; edc_queue_init(&queue);
```

### 4.6 验证

每个数据结构实现后，单独编译相关文件验证，再整体测试。

---

## Phase 5: 线程与同步 → libuv

**预估**: 2-3 天 | **风险**: 中

### 5.1 线程 API 映射

| GLib | libuv |
|------|-------|
| `GThread thread;` | `uv_thread_t thread;` |
| `g_thread_new("name", func, data)` | `uv_thread_create(&thread, func, data)` |
| `g_thread_join(thread)` | `uv_thread_join(&thread)` |
| `g_thread_self()` | `uv_thread_self()` |

**注意**: 
- libuv 的 `uv_thread_create` 不支持命名线程，需要时可用 `pthread_setname_np` 单独设置。
- `g_thread_join` 接受的是 `GThread*`，而 `uv_thread_join` 接受 `uv_thread_t*`。

### 5.2 互斥锁 API 映射

| GLib | libuv |
|------|-------|
| `GMutex mutex;` | `uv_mutex_t mutex;` |
| `g_mutex_init(&mutex)` | `uv_mutex_init(&mutex)` |
| `g_mutex_lock(&mutex)` | `uv_mutex_lock(&mutex)` |
| `g_mutex_unlock(&mutex)` | `uv_mutex_unlock(&mutex)` |
| `g_mutex_clear(&mutex)` | `uv_mutex_destroy(&mutex)` |

### 5.3 GAsyncQueue 替换

GAsyncQueue 是线程安全的消息队列。用 `uv_mutex_t` + EdcQueue 构建：

```c
typedef struct EdcAsyncQueue {
    EdcQueue queue;
    uv_mutex_t mutex;
} EdcAsyncQueue;

void  edc_async_queue_init(EdcAsyncQueue* q);
void  edc_async_queue_push(EdcAsyncQueue* q, void* data);
void* edc_async_queue_try_pop(EdcAsyncQueue* q);
void  edc_async_queue_destroy(EdcAsyncQueue* q);
```

### 5.4 涉及文件

- `edc_actor.c/h` — GThread, GAsyncQueue
- `edc_network_async.c/h` — GThread, GMutex
- `edc_channel_actor.c/h` — GThread, GAsyncQueue
- `edc_channel_cli.c` — GThread, GAsyncQueue
- `edc_log.c` — GMutex
- `edc_tools_shell.c` — GMutex
- `edc_tools_device_file.c` — GThread
- `edc_tools_device_shell.c` — GThread

### 5.5 验证

```bash
bazel build //src/agent
bazel test //tests:test_channel_actor //tests:test_llm_actor //tests:test_memory_actor
```

---

## Phase 6: 事件循环 → libuv（核心难点）

**预估**: 5-10 天 | **风险**: 高

这是整个迁移中最复杂、最核心的部分。GLib 的主循环模式和 libuv 的事件循环模式虽然有概念上的对应关系，但具体编程模型差异显著。

### 6.1 概念映射总览

| 概念 | GLib | libuv |
|------|------|-------|
| 事件循环 | `GMainLoop` | `uv_loop_t` |
| 执行上下文 | `GMainContext` | `uv_loop_t` |
| 事件源 | `GSource` | `uv_handle_t` 子类 |
| 空闲回调 | `g_idle_add()` | `uv_idle_t` |
| 定时器 | `g_timeout_add_seconds()` | `uv_timer_t` |
| IO 通道 | `GIOChannel` | `uv_pipe_t` / `uv_tty_t` / `uv_poll_t` |
| 信号处理 | `g_unix_signal_add()` | `uv_signal_t` |
| 一次性的工作 | — | `uv_work_t` (线程池) |

### 6.2 主循环

| GLib | libuv |
|------|-------|
| `loop = g_main_loop_new(ctx, FALSE)` | `loop = uv_default_loop()` 或 `uv_loop_new()` |
| `g_main_loop_run(loop)` | `uv_run(loop, UV_RUN_DEFAULT)` |
| `g_main_loop_quit(loop)` | `uv_stop(loop)` |
| `g_main_loop_is_running(loop)` | 自行追踪状态 |
| `g_main_context_get_thread_default()` | `uv_default_loop()` |
| `g_main_context_iteration(ctx, may_block)` | `uv_run(loop, UV_RUN_NOWAIT)` 或 `UV_RUN_ONCE` |

### 6.3 定时器

当前位置：`edc_tools_shell.c` (健康检查)，`edc_tools_crontab.c` (定时任务)

| GLib | libuv |
|------|-------|
| `g_timeout_add_seconds(interval, cb, data)` | `uv_timer_init(loop, &timer)` + `uv_timer_start(&timer, cb, interval*1000, 0)` |
| `g_source_destroy(src)` | `uv_timer_stop(&timer)` + `uv_close((uv_handle_t*)&timer, close_cb)` |

**libuv 的回调签名**:
```c
void timer_cb(uv_timer_t* handle) {
    // handle->data 存储用户数据
}
```

### 6.4 IO 监听 (CLI stdin、subprocess 管道)

当前靠 `GIOChannel` + `g_io_create_watch` / `g_io_add_watch` 实现。

#### stdin / 终端输入
| GLib | libuv |
|------|-------|
| `g_io_channel_unix_new(fd)` | `uv_pipe_init(loop, &pipe, 0)` + `uv_pipe_open(&pipe, fd)` |
| `g_io_add_watch(ch, cond, cb, data)` | `uv_read_start((uv_stream_t*)&pipe, alloc_cb, read_cb)` |
| `g_io_channel_read_line()` | 在 read_cb 中按行缓冲 |

**libuv read 回调模式**:
```c
void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread > 0) {
        // 处理 buf->base[0..nread-1]
    } else if (nread < 0) {
        // 错误或 EOF
    }
    free(buf->base);
}
```

#### subprocess stdout/stderr 管道
| GLib | libuv |
|------|-------|
| `g_io_channel_unix_new(pipe_fd)` | `uv_pipe_init(loop, &pipe, 0)` + `uv_pipe_open(&pipe, pipe_fd)` |
| `g_io_create_watch(ch, cond)` | 不需要，直接用 `uv_read_start` |

### 6.5 GSource 生命周期

| GLib | libuv |
|------|-------|
| `g_source_new(&funcs, sizeof(MySource))` | 直接初始化对应的 uv handle |
| `g_source_set_callback(src, cb, data, notify)` | 设置 handle->data 和回调函数 |
| `g_source_attach(src, ctx)` | handle 绑定到初始化时传入的 loop（自动） |
| `g_source_destroy(src)` | `uv_close((uv_handle_t*)handle, close_cb)` |
| `g_source_unref(src)` | — |
| `g_source_remove(tag)` | — |

**关键差异**:
- GLib 中 Source 可附加到不同的 Context，可在不同线程间移动
- libuv 中 handle 绑定到创建时的 loop，不能跨线程使用
- libuv 的资源释放是异步的：`uv_close` 不立即释放，close_cb 被调用时才安全释放

### 6.6 子进程管理

当前仅 `edc_tools_shell.c` 和测试文件使用 `g_spawn_async_with_pipes`。

| GLib | libuv |
|------|-------|
| `g_spawn_async_with_pipes(wd, argv, envp, flags, setup, data, &pid, &stdin_fd, &stdout_fd, &stderr_fd, &err)` | `uv_spawn(loop, &process, &options)` |
| `GPid pid` | `process.pid` |
| `g_spawn_close_pid(pid, NULL)` | `uv_close((uv_handle_t*)&process, close_cb)` 或 `uv_unref(&process)` |

**libuv 子进程选项**:
```c
uv_process_options_t options = {
    .file = "/bin/sh",           // 可执行文件
    .args = args,                 // 参数数组（NULL terminated）
    .env = NULL,                  // 环境变量
    .cwd = "/tmp",                // 工作目录
    .flags = UV_PROCESS_SETUID | UV_PROCESS_SETGID,
    .uid = uid,
    .gid = gid,
    .stdio_count = 3,
    .stdio = stdio,               // uv_stdio_container_t[3]
    .exit_cb = on_exit,           // 进程退出回调
};

// stdio 配置
uv_stdio_container_t stdio[3];
stdio[0].flags = UV_IGNORE;            // stdin
stdio[1].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;  // stdout
stdio[1].data.stream = (uv_stream_t*)&stdout_pipe;
stdio[2].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;  // stderr
stdio[2].data.stream = (uv_stream_t*)&stderr_pipe;
```

### 6.7 特殊处理：多线程场景

当前代码中每个 Actor 有自己的线程，线程内运行 GLib main loop。在 libuv 中：

- **每个线程应该有自己的 `uv_loop_t`**（通过 `uv_loop_new()` 创建，而非 `uv_default_loop()`）
- handle 不能跨线程使用
- 跨线程通信使用 `uv_async_t`：在一个线程中 `uv_async_send()`，另一个线程的 loop 中回调触发

```c
// 线程 A → 线程 B 通知
uv_async_t async;
uv_async_init(loop_of_thread_b, &async, async_cb);
// 在线程 A 中:
uv_async_send(&async);
```

### 6.8 迁移步骤建议

1. **先在 `edc_channel_cli.c` 做原型验证**：这是最简单的 channel，只涉及 stdin 监听和输出
2. **然后是 `edc_tools_shell.c`**：涉及子进程管理和管道监听
3. **再到 `edc_tools_crontab.c`**：涉及定时器
4. **最后迁移 `edc_actor.c`**：核心的 actor 线程模型
5. **channel 文件（qq, feishu, telegram）**：这些依赖 channel_actor 框架

### 6.9 验证

每个子步骤完成后编译+测试：
```bash
bazel build //src:edgeclaw
bazel test //tests:test_exec_cmdline_async
```

---

## Phase 7: 文件系统操作 → POSIX

**预估**: 2-3 天 | **风险**: 低

### 7.1 创建 `src/edc_fileutil.h` / `src/edc_fileutil.c`

| GLib | 替换实现 |
|------|---------|
| `g_file_get_contents(path, &buf, &len, &err)` | `edc_file_read_all(path, &buf, &len)` — fopen + fseek + fread |
| `g_file_set_contents(path, buf, len, &err)` | `edc_file_write_all(path, buf, len)` — 先写临时文件再 rename（原子写入） |
| `g_file_test(path, G_FILE_TEST_EXISTS)` | `access(path, F_OK) == 0` |
| `g_file_test(path, G_FILE_TEST_IS_DIR)` | `stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)` |
| `g_file_test(path, G_FILE_TEST_IS_REGULAR)` | `stat(path, &sb) == 0 && S_ISREG(sb.st_mode)` |
| `g_mkdir_with_parents(path, mode)` | `edc_mkdir_with_parents(path, mode)` — 递归创建目录 |
| `g_build_filename(a, b, ..., NULL)` | `edc_build_path(a, b, (const char*)NULL)` — 用 `/` 拼接 |
| `g_get_current_dir()` | `getcwd(buf, PATH_MAX)` |
| `g_get_home_dir()` | `getenv("HOME")` |
| `g_dir_open(path, 0, &err)` | `opendir(path)` |
| `g_dir_read_name(dir)` | `readdir(dir)->d_name` |
| `g_dir_close(dir)` | `closedir(dir)` |

### 7.2 涉及文件

- `edc_init_env.c` — g_file_get_contents, g_file_set_contents, g_file_test, g_get_current_dir, g_build_filename, g_mkdir_with_parents
- `edc_config.c` — g_file_get_contents, g_get_home_dir, g_build_filename, g_mkdir_with_parents
- `edc_tools_fs.c` — g_file_get_contents, g_file_set_contents, g_file_test, g_dir_*, g_build_filename, g_mkdir_with_parents
- `edc_history.c` — g_file_get_contents, g_file_test, g_build_filename
- `edc_secret.c` — g_file_get_contents, g_file_set_contents, g_build_filename
- `edc_memory_actor.c` — g_file_get_contents, g_file_set_contents, g_build_filename
- `edc_skills.c` — g_file_get_contents, g_dir_*, g_build_filename
- `edc_context.c` — g_file_get_contents, g_get_current_dir, g_build_filename
- `edc_log.c` — g_build_filename, g_mkdir_with_parents
- `edc_sandbox.c` — g_build_filename
- `edc_agent.c` — g_build_filename
- `edc_tools_crontab.c` — g_file_get_contents, g_file_test, g_build_filename
- `edc_tools_device_file.c` — g_file_test

### 7.3 GError 替换

GError 用于文件操作的错误传递。替换为简单的返回码模式：

```c
// 旧:
GError *error = NULL;
char *content;
g_file_get_contents(path, &content, NULL, &error);
if (error) { ... g_error_free(error); }

// 新:
char *content;
int err = edc_file_read_all(path, &content, NULL);
if (err != 0) { ... }
```

或者使用 `errno` 传递错误信息。

### 7.4 验证

```bash
bazel build //src/agent
bazel test //tests:test_memory_actor //tests:test_llm_actor //tests:test_channel_actor
```

---

## Phase 8: 杂项工具替换

**预估**: 2-3 天 | **风险**: 低

### 8.1 UUID 生成

| GLib | 替换 |
|------|------|
| `g_uuid_string_random()` | `edc_uuid_string_random()` — 读取 `/dev/urandom` 生成 UUID v4 字符串 |

使用位置：`edc_actor.c`, `edc_tools_crontab.c`

### 8.2 Base64 编解码

| GLib | 替换 |
|------|------|
| `g_base64_encode(data, len)` | `mbedtls_base64_encode(out, out_len, &olen, data, len)` |
| `g_base64_decode(text, &out_len)` | `mbedtls_base64_decode(out, out_len, &olen, text, len)` |

**mbedtls 已在项目依赖中** (`"@mbedtls//:mbedtls"`)。

使用位置：仅 `edc_tools_device_file.c`

### 8.3 UTF-8 处理

| GLib | 替换 |
|------|------|
| `g_utf8_make_valid(str, len)` | `edc_utf8_make_valid(str, len)` — 将无效字节替换为 U+FFFD |
| `g_utf8_get_char(p)` | `edc_utf8_get_char(p)` — 读取一个 UTF-8 码点，返回 `uint32_t` + 推进指针 |
| `g_utf8_next_char(p)` | `edc_utf8_next_char(p)` — 跳到下一个 UTF-8 字符起始位置 |
| `gunichar` | `uint32_t` |

使用位置：`edc_turn.c`, `edc_session.c` (g_utf8_make_valid), `edc_markdown_render.c` (g_utf8_get_char, g_utf8_next_char, gunichar)

### 8.4 日期时间 → POSIX time

| GLib | 替换 |
|------|------|
| `GDateTime*` | `struct tm` + `time_t` |
| `g_date_time_new_now_local()` | `time_t now = time(NULL); localtime_r(&now, &tm);` |
| `g_date_time_new_now_utc()` | `time_t now = time(NULL); gmtime_r(&now, &tm);` |
| `g_date_time_new_from_unix_local(ts)` | `time_t t = ts; localtime_r(&t, &tm);` |
| `g_date_time_new_from_iso8601(str, NULL)` | `edc_parse_iso8601(str)` — 自定义解析函数 |
| `g_date_time_format(dt, fmt)` | `strftime(buf, sizeof(buf), fmt, &tm)` |
| `g_date_time_unref(dt)` | 无需操作（值类型） |
| `GTimeZone*` | `timezone_t` (POSIX.1-2024) 或手动维护 tz 信息 |
| `g_get_real_time()` | `clock_gettime(CLOCK_REALTIME, &ts)` 或 `uv_hrtime()` |

使用位置：`edc_history.c` (GDateTime), `edc_tools_crontab.c` (GDateTime + GTimeZone + GTimeZone)

**注意**: `GTimeZone` 的时区切换功能较复杂（crontab 支持按用户设置的时区定时执行）。如果目标平台支持，可用 POSIX 的 `setenv("TZ", zone_id, 1); tzset();` 设置；否则需要自定义实现。

### 8.5 正则表达式

| GLib | 替换 |
|------|------|
| `GRegex*` | POSIX `regex_t` (`<regex.h>`) |
| `g_regex_new(pattern, 0, 0, &err)` | `regcomp(&regex, pattern, REG_EXTENDED)` |
| `g_regex_match(regex, str, 0, &match_info)` | `regexec(&regex, str, nmatch, pmatch, 0)` |
| `GMatchInfo*` | `regmatch_t pmatch[N]` |
| `g_match_info_fetch(mi, group)` | 使用 `pmatch[group]` 从原字符串切片 |
| `g_match_info_free(mi)` | 无需操作 |
| `g_regex_unref(regex)` | `regfree(&regex)` |

使用位置：仅 `edc_skills.c`

### 8.6 usleep / time

| GLib | 替换 |
|------|------|
| `g_usleep(n)` | `usleep(n)` (`<unistd.h>`) |
| `G_USEC_PER_SEC` | `1000000` 或 `USEC_PER_SEC` |

使用位置：`main.c`, `tests/test_mongoose_https_client.c`

### 8.7 g_print / g_printerr（仅测试文件）

| GLib | 替换 |
|------|------|
| `g_print(fmt, ...)` | `printf(fmt, ...)` |
| `g_printerr(fmt, ...)` | `fprintf(stderr, fmt, ...)` |

使用位置：仅测试文件中的 debug 输出。

### 8.8 验证

```bash
bazel build //src/agent
bazel test //tests/...
```

---

## Phase 9: 清理收尾

**预估**: 1-2 天 | **风险**: 低

### 9.1 代码层面

1. 确认所有 `#include <glib.h>` 已移除
2. 确认不再有任何 GLib 类型/API 引用
3. 搜索确保无残留：`grep -r "glib\|gchar\|gboolean\|g_free\|g_new\|GString\|GHashTable\|GList\|GQueue\|GMainLoop\|GSource\|GError\|g_thread\|g_mutex\|g_spawn\|g_dir\|g_file_get\|g_strdup_printf"`

### 9.2 构建系统

1. 从 `MODULE.bazel` 移除：
   ```python
   bazel_dep(name = "glib", version = "2.82.2.bcr.8")
   single_version_override(module_name = "glib", ...)
   ```
2. 从 `src/BUILD` 移除 `"@glib//glib:glib"` (agent 和 edc_secret_lib)
3. 从 `tests/BUILD` 移除所有 `"@glib//glib:glib"` (9处)
4. 删除 `patches/glib-gmodule-fix.patch`, `patches/glib-atomic-lock-free.patch`
5. 更新 `patches/BUILD.bazel` 移除对应条目

### 9.3 全平台构建验证

```bash
# amd64 Linux
bazel build //...

# ARM Linux (交叉编译)
bazel build //... --platforms=//:arm_linux

# ARM64 Linux (交叉编译)
bazel build //... --platforms=//:aarch64_linux

# Android (交叉编译)
bazel build //... --platforms=//:arm_android
bazel build //... --platforms=//:aarch64_android
```

### 9.4 测试验证

```bash
bazel test //tests/...
```

---

## 附录 A: 风险清单

| 风险 | 等级 | 缓解措施 |
|------|------|---------|
| 事件循环行为语义差异导致死锁/竞态 | 高 | Phase 6 先在简单模块原型验证，充分测试 |
| 多线程 Actor 模型在 libuv 下表现不一致 | 高 | 每个线程用独立 uv_loop，uv_async 做跨线程通信 |
| GString → SDS 的 API 语义差异（g_string_free FALSE 模式等） | 中 | 创建薄封装层，逐文件验证 |
| GTimeZone 复杂时区操作难以用 POSIX 替代 | 中 | crontab 是唯一重度使用者，评估是否可用 setenv+TZ |
| libuv 的异步资源释放模式（uv_close 不立即释放） | 中 | 仔细管理生命周期，确保 close_cb 中才释放内存 |
| 数据结构替换引入 bug | 中 | 充分单元测试，特别是哈希表和 GQueue |
| GError 错误传递模式改变 | 低 | 统一错误处理约定 |
| 跨平台兼容性（Android） | 低 | 每个 Phase 后交叉编译验证 |

## 附录 B: 文件迁移状态跟踪表

可在实施过程中用于跟踪进度：

| 文件 | Phase1 | Phase2 | Phase3 | Phase4 | Phase5 | Phase6 | Phase7 | Phase8 | 移除 glib.h |
|------|--------|--------|--------|--------|--------|--------|--------|--------|------------|
| src/main.c | | | | | | | | | |
| src/edc_actor.c/h | | | | | | | | | |
| src/edc_agent.c/h | | | | | | | | | |
| src/edc_channel_actor.c/h | | | | | | | | | |
| src/edc_channel_cli.c/h | | | | | | | | | |
| src/edc_channel_feishu.c/h | | | | | | | | | |
| src/edc_channel_qq.c/h | | | | | | | | | |
| src/edc_channel_telegram.c/h | | | | | | | | | |
| src/edc_commands.c/h | | | | | | | | | |
| src/edc_config.c/h | | | | | | | | | |
| src/edc_context.c/h | | | | | | | | | |
| src/edc_history.c/h | | | | | | | | | |
| src/edc_init_env.c/h | | | | | | | | | |
| src/edc_llm_actor.c/h | | | | | | | | | |
| src/edc_log.c/h | | | | | | | | | |
| src/edc_lwip.c/h | | | | | | | | | |
| src/edc_markdown_render.c/h | | | | | | | | | |
| src/edc_memory_actor.c/h | | | | | | | | | |
| src/edc_network_async.c/h | | | | | | | | | |
| src/edc_ns_sandbox.c/h | | | | | | | | | |
| src/edc_param_types.c/h | | | | | | | | | |
| src/edc_sandbox.c/h | | | | | | | | | |
| src/edc_secret.c/h | | | | | | | | | |
| src/edc_session.c/h | | | | | | | | | |
| src/edc_skills.c/h | | | | | | | | | |
| src/edc_toolbox.c/h | | | | | | | | | |
| src/edc_toolbox_actor.c/h | | | | | | | | | |
| src/edc_tools.c/h | | | | | | | | | |
| src/edc_tools_crontab.c/h | | | | | | | | | |
| src/edc_tools_device_file.c/h | | | | | | | | | |
| src/edc_tools_device_shell.c/h | | | | | | | | | |
| src/edc_tools_fs.c/h | | | | | | | | | |
| src/edc_tools_shell.c/h | | | | | | | | | |
| src/edc_tools_todo.c/h | | | | | | | | | |
| src/edc_turn.c/h | | | | | | | | | |
| tests/test_channel_actor.c | | | | | | | | | |
| tests/test_exec_cmdline_async.c | | | | | | | | | |
| tests/test_llm_actor.c | | | | | | | | | |
| tests/test_lwip_http.c | | | | | | | | | |
| tests/test_markdown_render.c | | | | | | | | | |
| tests/test_memory_actor.c | | | | | | | | | |
| tests/test_mongoose_https_client.c | | | | | | | | | |
| tests/test_mongoose_resolve.c | | | | | | | | | |
| tests/test_pppos_agent_stress.c | | | | | | | | | |

---

## 附录 C: 新增文件清单

迁移完成后，项目将新增以下文件：

| 文件 | 用途 | Phase |
|------|------|-------|
| `src/edc_compat.h` | 类型别名 + 内存宏 | 0 |
| `src/edc_strutil.h` / `src/edc_strutil.c` | 字符串工具函数 | 2 |
| `src/edc_containers.h` / `src/edc_containers.c` | 数据结构 (HashTable, Array, List, Queue) | 4 |
| `src/edc_fileutil.h` / `src/edc_fileutil.c` | 文件系统操作 | 7 |
| `src/edc_utf8.h` / `src/edc_utf8.c` | UTF-8 处理函数 | 8 |

---

## 附录 D: 构建依赖变更

### 新增依赖
```python
bazel_dep(name = "libuv", version = "1.48.0")  # Phase 0
```

### 移除依赖
```python
# 移除以下两段：
bazel_dep(name = "glib", version = "2.82.2.bcr.8")
single_version_override(module_name = "glib", ...)
```

### 移除文件
```
patches/glib-gmodule-fix.patch
patches/glib-atomic-lock-free.patch
```

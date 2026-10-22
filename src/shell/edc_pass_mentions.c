#include "edc_passes.h"

#include "edc_error.h"
#include "edc_mention.h"
#include "edc_file_utils.h"
#include "edc_sandbox.h"

#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* 一次引用的上限。文件是**用户显式引用**的，所以给得比"模型自己 read"宽松一点，但仍然
 * 必须有界：一条消息炸掉上下文比少读两行严重得多。 */
#define EDC_MENTION_MAX_TOKEN 4096
#define EDC_MENTION_MAX_FILE_LINES 2000
#define EDC_MENTION_MAX_FILE_BYTES (64 * 1024)
#define EDC_MENTION_MAX_TOTAL_BYTES (128 * 1024)
/* 目录清单的条数上限。清单是给模型判断"这个目录里有什么方向"的，不是内容本身：200 条足够，
 * 再多只是烧总预算（一个 Build 产物目录可以轻松上万条）。 */
#define EDC_MENTION_MAX_DIR_ENTRIES 200

struct mention_state
{
    const struct edc_pass_ctx *ctx;
    sds blocks; /* 这一遍累积出的正文（NULL = 还没有）；最后作为一条 part 追加 */
    size_t total;
    sds *seen; /* 已展开的 "safe#start-end"，同一载荷内去重 */
    size_t nseen;
    size_t cseen;
};

/* Whitespace and the reference grammar itself live in edc_mention.h: the TUI paints
 * references with the same code, so the two cannot disagree about where one ends. */
#define is_space edc_mention_is_space

/* JSON 字符串里的最小转义（路径只可能出现 " 与 \ 与个别控制字符）。 */
static sds json_escape(const char *s)
{
    sds out = sdsempty();
    for (const char *p = s; p && *p; p++)
    {
        if (*p == '"' || *p == '\\')
            out = sdscatprintf(out, "\\%c", *p);
        else if ((unsigned char)*p < 0x20)
            out = sdscatprintf(out, "\\u%04x", (unsigned)(unsigned char)*p);
        else
            out = sdscatlen(out, p, 1);
    }
    return out;
}

static bool seen_before(struct mention_state *st, const char *key)
{
    for (size_t i = 0; i < st->nseen; i++)
        if (strcmp(st->seen[i], key) == 0)
            return true;
    return false;
}

static void mark_seen(struct mention_state *st, sds key /* 所有权转移 */)
{
    if (st->nseen == st->cseen)
    {
        st->cseen = st->cseen ? st->cseen * 2 : 4;
        st->seen = realloc(st->seen, st->cseen * sizeof(*st->seen));
    }
    st->seen[st->nseen++] = key;
}

/* 与 read 工具的结果同形："Called the Read tool …" 头 + 正文。模型看到的就是
 * "用户引用了这个文件"，再拿到工具本来会给它的那一段。 */
static sds block_header(const char *display, int start, int end)
{
    sds j = json_escape(display);
    sds h;
    if (start <= 0)
        h = sdscatprintf(sdsempty(), "Called the Read tool with the following input: {\"path\":\"%s\"}", j);
    else if (end >= start)
        h = sdscatprintf(sdsempty(),
                         "Called the Read tool with the following input: {\"path\":\"%s\",\"offset\":%d,\"limit\":%d}",
                         j, start, end - start + 1);
    else
        h = sdscatprintf(sdsempty(), "Called the Read tool with the following input: {\"path\":\"%s\",\"offset\":%d}",
                         j, start);
    sdsfree(j);
    return h;
}

/* 1 基、含两端的行切片；|end| < |start| 表示到文件结尾。 */
static sds slice_lines(const char *content, int start, int end)
{
    const char *p = content;
    const char *stop = content + strlen(content);

    int line = 1;
    while (*p && line < start)
    {
        const char *nl = strchr(p, '\n');
        if (!nl)
            return sdsempty(); /* 文件比 start 短 */
        p = nl + 1;
        line++;
    }
    const char *begin = p;

    if (end >= start)
    {
        line = start;
        while (p < stop && line <= end)
        {
            const char *nl = strchr(p, '\n');
            if (!nl)
            {
                p = stop;
                break;
            }
            p = nl + 1;
            line++;
        }
        stop = p;
    }
    return sdsnewlen(begin, (size_t)(stop - begin));
}

/* 行数/字节上限。截断总是落在行边界上（除非文件本身没有换行）。 */
static sds cap_content(sds content)
{
    bool truncated = false;

    int lines = 0;
    for (const char *p = content; *p; p++)
    {
        if (*p != '\n')
            continue;
        if (++lines == EDC_MENTION_MAX_FILE_LINES)
        {
            sdsrange(content, 0, (ssize_t)(p - content));
            truncated = true;
            break;
        }
    }

    if (sdslen(content) > EDC_MENTION_MAX_FILE_BYTES)
    {
        size_t cut = EDC_MENTION_MAX_FILE_BYTES;
        while (cut > 0 && content[cut] != '\n')
            cut--;
        if (cut == 0)
            cut = EDC_MENTION_MAX_FILE_BYTES;
        sdsrange(content, 0, (ssize_t)cut);
        truncated = true;
    }

    if (truncated)
        content = sdscat(content, "… [truncated]\n");
    return content;
}

struct list_ctx
{
    sds out;
    int n;           /* 已写入的条目数 */
    bool truncated;  /* 撞到 EDC_MENTION_MAX_DIR_ENTRIES */
};

static void list_cb(const char *name, int type, void *userdata)
{
    struct list_ctx *c = userdata;
    if (c->n >= EDC_MENTION_MAX_DIR_ENTRIES)
    {
        c->truncated = true;
        return;
    }
    c->out = sdscatprintf(c->out, "%s%s\n", (type == UV_DIRENT_DIR) ? "[DIR]  " : "[FILE] ", name);
    c->n++;
}

static sds failed_block(const char *display, const char *reason)
{
    return sdscatprintf(sdsempty(), "Read tool failed to read %s: %s", display, reason ? reason : "unknown error");
}

/* 读一个引用，返回一段 block（NULL = 什么都没加，例如重复引用）。
 * |safe| 已过沙箱/路径校验，|display| 是用户写的那个相对路径。 */
static sds build_block(struct mention_state *st, const char *display, const char *safe, int start, int end)
{
    sds body = NULL;

    if (edc_file_is_dir(safe))
    {
        struct list_ctx lc = {.out = sdsempty()};
        EdcError *err = NULL;
        if (!edc_dir_foreach(safe, list_cb, &lc, &err))
        {
            sds block = failed_block(display, err ? err->message : NULL);
            if (err)
                edc_error_free(err);
            sdsfree(lc.out);
            return block;
        }
        if (lc.truncated)
            lc.out = sdscat(lc.out, "… [truncated]\n");
        body = lc.out;
        /* A line range cannot apply to a LISTING: drop it, so the header below does not
         * advertise an offset/limit the body does not honour (a model told "offset 2,
         * limit 2" would read the listing as those lines). */
        start = 0;
        end = 0;
    }
    else
    {
        EdcError *err = NULL;
        sds content = edc_file_read(safe, &err);
        if (!content)
        {
            sds block = failed_block(display, err ? err->message : NULL);
            if (err)
                edc_error_free(err);
            return block;
        }

        /* 二进制不附体：模型拿到一段乱码比拿到一行说明有害得多。 */
        const size_t probe = sdslen(content) < 512 ? sdslen(content) : 512;
        if (memchr(content, '\0', probe))
        {
            sdsfree(content);
            return sdscatprintf(sdsempty(), "Read tool skipped %s: binary file", display);
        }

        if (start > 0)
        {
            sds sliced = slice_lines(content, start, end);
            sdsfree(content);
            content = sliced;
        }
        body = cap_content(content);
    }

    sds header = block_header(display, start, end);
    sds block = sdscatprintf(sdsempty(), "%s\n", header);
    sdsfree(header);
    block = sdscatsds(block, body);
    sdsfree(body);
    return block;
}

/* 解析 `path` / `path#12` / `path#12-20`。后缀不合法时整段当路径（文件名里可以有 '#'）。 */
static void split_range(const char *tok, sds *base, int *start, int *end)
{
    *start = 0;
    *end = 0;
    *base = sdsnew(tok);

    const char *hash = strrchr(tok, '#');
    if (!hash || !hash[1])
        return;
    const char *r = hash + 1;
    for (const char *q = r; *q; q++)
        if ((*q < '0' || *q > '9') && *q != '-')
            return;

    char *endp = NULL;
    long s = strtol(r, &endp, 10);
    if (s < 1 || !endp)
        return;
    long e = 0;
    if (*endp == '-')
    {
        const char *after = endp + 1;
        if (*after)
        {
            char *endp2 = NULL;
            e = strtol(after, &endp2, 10);
            if (e < s)
                return;
        }
    }

    sdsfree(*base);
    *base = sdsnewlen(tok, (size_t)(hash - tok));
    *start = (int)s;
    *end = (int)e;
}

/* 一个引用 → 一段 block。失败一律变成一行说明（输入照常执行）。 */
static sds render_mention(struct mention_state *st, const char *token, size_t len)
{
    if (len == 0 || len > EDC_MENTION_MAX_TOKEN)
        return NULL;

    sds tok = sdsnewlen(token, len);
    sds base = NULL;
    int start = 0, end = 0;
    split_range(tok, &base, &start, &end);
    sdsfree(tok);

    if (sdslen(base) == 0)
    {
        sdsfree(base);
        return NULL;
    }

    EdcError *err = NULL;
    sds safe = NULL;
    if (st->ctx->conf)
    {
        safe = edc_sandbox_verify_path_at(st->ctx->conf, st->ctx->workdir, base, false, &err);
    }
    else
    {
        /* No config (unit-test / embedding path): still resolve against the workdir, so a
         * relative reference means what the user's session means by it. */
        if (st->ctx->workdir && st->ctx->workdir[0] && base[0] != '/')
            safe = edc_path_join(st->ctx->workdir, base, NULL);
        else
            safe = sdsnew(base);
    }
    if (!safe)
    {
        sds block = failed_block(base, err ? err->message : NULL);
        if (err)
            edc_error_free(err);
        sdsfree(base);
        return block;
    }

    sds key = sdscatprintf(sdsempty(), "%s#%d-%d", safe, start, end);
    if (seen_before(st, key))
    {
        sdsfree(key);
        sdsfree(safe);
        sdsfree(base);
        return NULL;
    }
    mark_seen(st, key);

    /* 整条消息的附加内容预算：超了就不再读，只说明一句。 */
    if (st->total >= EDC_MENTION_MAX_TOTAL_BYTES)
    {
        sds block = sdscatprintf(sdsempty(), "Read tool skipped %s: attachment budget exhausted", base);
        sdsfree(safe);
        sdsfree(base);
        return block;
    }

    sds block = build_block(st, base, safe, start, end);
    sdsfree(safe);
    sdsfree(base);
    return block;
}

static void append_block(struct mention_state *st, sds block)
{
    if (!block)
        return;
    if (!st->blocks)
        st->blocks = sdsempty();
    else
        st->blocks = sdscatlen(st->blocks, "\n\n", 2);
    st->blocks = sdscatsds(st->blocks, block);
    st->total += sdslen(block);
    sdsfree(block);
}

/* 扫一遍载荷文本：去掉 `\@` 的反斜杠，把每次 `@引用` 展开成 block。 */
static bool expand_payload(const struct edc_pass_ctx *ctx, struct edc_msg_payload *payload)
{
    if (!payload || !payload->text || payload->text[0] == '\0')
        return true;
    if (!strchr(payload->text, '@'))
        return true; /* 走开：绝大多数消息一个字节都不用复制 */

    /* 预算按"载荷已有的附加内容 + 这一遍"算：passes 是按注册顺序跑的，前面的 pass
     * 可能已经追加过消息。 */
    struct mention_state st = {.ctx = ctx, .blocks = NULL, .total = 0};
    for (int i = 0; i < payload->nparts; i++)
        st.total += sdslen(payload->parts[i].content);

    sds out = sdsempty();
    /* |src| stays valid for the whole scan; |out| is built alongside and replaces
     * payload->text only at the end, so every |p| still points into the original. */
    const char *src = payload->text;
    const char *p = src;
    while (*p)
    {
        if (p[0] == '\\' && p[1] == '@')
        {
            out = sdscatlen(out, "@", 1); /* 转义：字面量 @，不展开 */
            p += 2;
            continue;
        }

        /* A reference (`@path`, `@"my file.txt"`, …): edc_mention_parse owns where it ends
         * and what it names -- quotes are delimiters there, exactly as everywhere else.
         * The TEXT keeps the bytes the user typed (including the quotes and any trailing
         * punctuation the reference gave back). */
        if (*p == '@' && (p == src || is_space(p[-1])))
        {
            sds token = NULL;
            const size_t mlen = edc_mention_parse(src, (size_t)(p - src), &token, NULL);
            if (mlen > 0 && token)
            {
                /* render_mention takes the token to resolve; it may also carry a `#range`
                 * suffix, which split_range() understands. */
                append_block(&st, render_mention(&st, token, sdslen(token)));
            }
            sdsfree(token);
            if (mlen > 0)
            {
                out = sdscatlen(out, p, mlen);
                p += mlen;
                continue;
            }
        }

        out = sdscatlen(out, p, 1);
        p++;
    }

    sdsfree(payload->text);
    payload->text = out;

    /* 这一遍的所有引用合成**一条**消息（块之间空行分隔）：模型看到的顺序就是
     * 用户文本 -> 这些读取结果。想拆成多条就多加几个 add_part。 */
    if (st.blocks)
    {
        edc_msg_payload_add_part(payload, EDC_MSG_USER, true, st.blocks);
        sdsfree(st.blocks);
    }

    for (size_t i = 0; i < st.nseen; i++)
        sdsfree(st.seen[i]);
    free(st.seen);
    return true;
}

bool edc_pass_mentions(const struct edc_pass_ctx *ctx, edc_ast *ast, sds *out_error)
{
    if (out_error)
        *out_error = NULL;
    if (!ctx || !ast)
        return true;

    /* 每个"消息载荷"走同一条路：MESSAGE 节点一条，SHELL 每个 stage 一条。 */
    if (ast->kind == EDC_AST_MESSAGE)
        return expand_payload(ctx, &ast->message);

    for (struct edc_ast_pipeline *p = ast->pipelines; p; p = p->next)
        for (struct edc_ast_stage *s = p->stages; s; s = s->next)
            if (!expand_payload(ctx, &s->msg))
                return false;
    return true;
}

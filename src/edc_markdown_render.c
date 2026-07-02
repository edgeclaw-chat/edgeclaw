#include "edc_markdown_render.h"
#include "md4c.h"
#include "log.h"
#include "edc_compat.h"
#include "sds.h"
#include "utf8proc.h"
#include <stdlib.h>
#include <string.h>

struct edc_ptr_array {
    void **pdata;
    unsigned int len;
    unsigned int cap;
};

static struct edc_ptr_array *edc_ptr_array_new(void)
{
    struct edc_ptr_array *arr = EDC_NEW0(struct edc_ptr_array, 1);
    arr->cap = 4;
    arr->pdata = EDC_NEW(void *, arr->cap);
    return arr;
}

static void edc_ptr_array_add(struct edc_ptr_array *arr, void *ptr)
{
    if (arr->len >= arr->cap) {
        arr->cap *= 2;
        arr->pdata = realloc(arr->pdata, arr->cap * sizeof(void *));
    }
    arr->pdata[arr->len++] = ptr;
}

static void *edc_ptr_array_index(struct edc_ptr_array *arr, unsigned int idx)
{
    return arr->pdata[idx];
}

static void edc_ptr_array_free(struct edc_ptr_array *arr)
{
    free(arr->pdata);
    free(arr);
}

#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_DIM        "\033[2m"
#define ANSI_ITALIC     "\033[3m"
#define ANSI_UNDERLINE  "\033[4m"
#define ANSI_STRIKE     "\033[9m"

#define ANSI_BLACK      "\033[30m"
#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_WHITE      "\033[37m"
#define ANSI_GRAY       "\033[90m"

#define ANSI_BOLD_RED       "\033[1;31m"
#define ANSI_BOLD_GREEN     "\033[1;32m"
#define ANSI_BOLD_YELLOW    "\033[1;33m"
#define ANSI_BOLD_BLUE      "\033[1;34m"
#define ANSI_BOLD_MAGENTA   "\033[1;35m"
#define ANSI_BOLD_CYAN      "\033[1;36m"

static const char *HEADING_COLORS[] = {
    NULL,
    ANSI_BOLD_BLUE,
    ANSI_BOLD_YELLOW,
    ANSI_BOLD_GREEN,
    ANSI_BOLD_MAGENTA,
    ANSI_BOLD_CYAN,
    ANSI_BOLD_RED,
};

#define MAX_LIST_DEPTH 16

struct edc_md_render_ctx {
    sds out;
    bool in_table;
    struct edc_ptr_array *table_rows;
    struct edc_ptr_array *current_row;
    sds current_cell;
    struct edc_ptr_array *col_aligns;
    int td_col_idx;
    int list_depth;
    int ol_counter[MAX_LIST_DEPTH];
    bool is_ordered[MAX_LIST_DEPTH];
    bool is_tight_list[MAX_LIST_DEPTH];
    bool in_quote;
    bool in_span_a;
    bool has_content;
};

static void edc_md_render_ctx_init(struct edc_md_render_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->out = sdsempty();
}

static int edc_md_visual_strlen(const char *s)
{
    int w = 0;
    bool in_esc = FALSE;
    const unsigned char *p = (const unsigned char *)s;
    ssize_t len = (ssize_t)strlen(s);

    while (p - (const unsigned char *)s < len) {
        utf8proc_int32_t uc;
        ssize_t clen = utf8proc_iterate(p, len - (p - (const unsigned char *)s), &uc);
        if (clen <= 0) break;
        if (uc == '\033') {
            in_esc = TRUE;
        } else if (in_esc) {
            if (uc == 'm')
                in_esc = FALSE;
        } else {
            int wv = utf8proc_charwidth(uc);
            if (wv == 1 && utf8proc_charwidth_ambiguous(uc))
                w += 2;
            else if (wv > 0)
                w += wv;
        }
        p += clen;
    }
    return w;
}

static void edc_md_render_ctx_free(struct edc_md_render_ctx *ctx)
{
    if (ctx->out)
        sdsfree(ctx->out);
    if (ctx->table_rows) {
        for (unsigned int i = 0; i < ctx->table_rows->len; i++) {
            struct edc_ptr_array *row = edc_ptr_array_index(ctx->table_rows, i);
            for (unsigned int j = 0; j < row->len; j++) {
                sds cell = edc_ptr_array_index(row, j);
                sdsfree(cell);
            }
            edc_ptr_array_free(row);
        }
        edc_ptr_array_free(ctx->table_rows);
    }
    if (ctx->current_row) {
        for (unsigned int i = 0; i < ctx->current_row->len; i++) {
            sds cell = edc_ptr_array_index(ctx->current_row, i);
            sdsfree(cell);
        }
        edc_ptr_array_free(ctx->current_row);
    }
    if (ctx->current_cell) {
        sdsfree(ctx->current_cell);
    }
    if (ctx->col_aligns) {
        for (unsigned int i = 0; i < ctx->col_aligns->len; i++) {
            int *a = edc_ptr_array_index(ctx->col_aligns, i);
            free(a);
        }
        edc_ptr_array_free(ctx->col_aligns);
    }
}

static int edc_md_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size,
                       void *userdata);

static int edc_md_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    struct edc_md_render_ctx *ctx = userdata;
    sds *o = &ctx->out;

    switch (type) {
    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_QUOTE:
        ctx->in_quote = TRUE;
        *o = sdscat(*o, ANSI_DIM "│ " ANSI_RESET);
        break;

    case MD_BLOCK_UL:
        if (ctx->list_depth < MAX_LIST_DEPTH) {
            ctx->ol_counter[ctx->list_depth] = 0;
            ctx->is_ordered[ctx->list_depth] = FALSE;
            ctx->is_tight_list[ctx->list_depth] =
                ((MD_BLOCK_UL_DETAIL *)detail)->is_tight;
        }
        ctx->list_depth++;
        if (ctx->has_content) {
            *o = sdscatlen(*o, &(char){'\n'}, 1);
        }
        break;

    case MD_BLOCK_OL: {
        if (ctx->list_depth < MAX_LIST_DEPTH) {
            MD_BLOCK_OL_DETAIL *det = detail;
            ctx->ol_counter[ctx->list_depth] = det->start;
            ctx->is_ordered[ctx->list_depth] = TRUE;
            ctx->is_tight_list[ctx->list_depth] = det->is_tight;
        }
        ctx->list_depth++;
        if (ctx->has_content) {
            *o = sdscatlen(*o, &(char){'\n'}, 1);
        }
        break;
    }

    case MD_BLOCK_LI: {
        int depth = ctx->list_depth - 1;
        if (depth < 0) depth = 0;
        if (depth >= MAX_LIST_DEPTH) depth = MAX_LIST_DEPTH - 1;

        *o = sdscatprintf(*o, "%*s", depth * 2, "");

        MD_BLOCK_LI_DETAIL *det = detail;
        if (det->is_task) {
            *o = sdscatprintf(*o, "%s[%c]%s ", ANSI_GREEN,
                                   det->task_mark ? det->task_mark : ' ',
                                   ANSI_RESET);
        } else if (depth < MAX_LIST_DEPTH && ctx->is_ordered[depth]) {
            *o = sdscatprintf(*o, "%s%d.%s ",
                                   ANSI_BOLD, ctx->ol_counter[depth]++,
                                   ANSI_RESET);
        } else {
            *o = sdscat(*o, "• ");
        }
        break;
    }

    case MD_BLOCK_HR:
        if (ctx->has_content) *o = sdscatlen(*o, &(char){'\n'}, 1);
        *o = sdscat(*o, ANSI_DIM "────" ANSI_RESET "\n");
        ctx->has_content = TRUE;
        break;

    case MD_BLOCK_H: {
        MD_BLOCK_H_DETAIL *det = detail;
        int level = det->level;
        if (level < 1) level = 1;
        if (level > 6) level = 6;
        if (ctx->has_content) *o = sdscatlen(*o, &(char){'\n'}, 1);
        *o = sdscat(*o, HEADING_COLORS[level]);
        for (int i = 0; i < level; i++)
            *o = sdscatlen(*o, &(char){'#'}, 1);
        *o = sdscatlen(*o, &(char){' '}, 1);
        break;
    }

    case MD_BLOCK_CODE:
        if (ctx->has_content) *o = sdscatlen(*o, &(char){'\n'}, 1);
        *o = sdscat(*o, ANSI_GRAY);
        {
            MD_BLOCK_CODE_DETAIL *det = detail;
            if (det->lang.text && det->lang.size > 0) {
                *o = sdscatlen(*o, det->lang.text, det->lang.size);
                *o = sdscatlen(*o, &(char){'\n'}, 1);
            }
        }
        break;

    case MD_BLOCK_HTML:
        break;

    case MD_BLOCK_P:
        if (ctx->has_content && !ctx->in_quote && ctx->list_depth == 0)
            *o = sdscatlen(*o, &(char){'\n'}, 1);
        break;

    case MD_BLOCK_TABLE:
        ctx->in_table = TRUE;
        ctx->table_rows = edc_ptr_array_new();
        ctx->col_aligns = edc_ptr_array_new();
        if (ctx->has_content) *o = sdscatlen(*o, &(char){'\n'}, 1);
        break;

    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        break;

    case MD_BLOCK_TR:
        ctx->current_row = edc_ptr_array_new();
        ctx->td_col_idx = 0;
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        ctx->current_cell = sdsempty();
        break;

    default:
        break;
    }

    return 0;
}

static int edc_md_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    struct edc_md_render_ctx *ctx = userdata;
    sds *o = &ctx->out;

    switch (type) {
    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_QUOTE:
        ctx->in_quote = FALSE;
        *o = sdscatlen(*o, &(char){'\n'}, 1);
        ctx->has_content = TRUE;
        break;

    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        if (ctx->list_depth > 0) ctx->list_depth--;
        if (ctx->list_depth == 0) {
            *o = sdscatlen(*o, &(char){'\n'}, 1);
        }
        ctx->has_content = TRUE;
        break;

    case MD_BLOCK_LI:
        if (!ctx->is_tight_list[ctx->list_depth > 0 ?
                               ctx->list_depth - 1 : 0]) {
            *o = sdscatlen(*o, &(char){'\n'}, 1);
        } else {
            if (ctx->list_depth > 0) *o = sdscatlen(*o, &(char){'\n'}, 1);
        }
        ctx->has_content = TRUE;
        break;

    case MD_BLOCK_HR:
        break;

    case MD_BLOCK_H: {
        MD_BLOCK_H_DETAIL *det = detail;
        int level = det->level;
        if (level < 1) level = 1;
        if (level > 6) level = 6;
        *o = sdscat(*o, ANSI_RESET "\n");
        if (level <= 2) *o = sdscatlen(*o, &(char){'\n'}, 1);
        ctx->has_content = TRUE;
        break;
    }

    case MD_BLOCK_CODE:
        *o = sdscat(*o, ANSI_RESET "\n\n");
        ctx->has_content = TRUE;
        break;

    case MD_BLOCK_HTML:
        *o = sdscatlen(*o, &(char){'\n'}, 1);
        ctx->has_content = TRUE;
        break;

    case MD_BLOCK_P:
        if (!ctx->in_quote) *o = sdscat(*o, ANSI_RESET);
        *o = sdscatlen(*o, &(char){'\n'}, 1);
        ctx->has_content = TRUE;
        break;

    case MD_BLOCK_TABLE: {
        ctx->in_table = FALSE;

        if (ctx->table_rows && ctx->table_rows->len > 0) {
            struct edc_ptr_array *rows = ctx->table_rows;
            MD_BLOCK_TABLE_DETAIL *det = detail;
            unsigned int ncols = det->col_count;
            unsigned int nhead = det->head_row_count;
            if (ncols == 0) ncols = 1;

            int *widths = EDC_NEW0(int, ncols);
            for (unsigned int r = 0; r < rows->len; r++) {
                struct edc_ptr_array *row = edc_ptr_array_index(rows, r);
                for (unsigned int c = 0; c < ncols && c < row->len; c++) {
                    sds cell = edc_ptr_array_index(row, c);
                    int w = edc_md_visual_strlen(cell);
                    if (w > widths[c]) widths[c] = w;
                }
            }
            for (unsigned int c = 0; c < ncols; c++) {
                if (widths[c] < 2) widths[c] = 2;
            }

            *o = sdscat(*o, ANSI_DIM);
            *o = sdscat(*o, "┌");
            for (unsigned int c = 0; c < ncols; c++) {
                for (int i = 0; i < widths[c] + 2; i++)
                    *o = sdscat(*o, "─");
                if (c < ncols - 1) *o = sdscat(*o, "┬");
            }
            *o = sdscat(*o, "┐\n" ANSI_RESET);

            for (unsigned int r = 0; r < rows->len; r++) {
                struct edc_ptr_array *row = edc_ptr_array_index(rows, r);

                *o = sdscat(*o, ANSI_DIM "│" ANSI_RESET);
                for (unsigned int c = 0; c < ncols; c++) {
                    *o = sdscatlen(*o, &(char){' '}, 1);
                    if (c < row->len) {
                        sds cell = edc_ptr_array_index(row, c);
                        *o = sdscat(*o, cell);
                        int len = edc_md_visual_strlen(cell);
                        for (int i = len; i < widths[c]; i++)
                            *o = sdscatlen(*o, &(char){' '}, 1);
                    } else {
                        for (int i = 0; i < widths[c]; i++)
                            *o = sdscatlen(*o, &(char){' '}, 1);
                    }
                    *o = sdscat(*o, ANSI_DIM " │" ANSI_RESET);
                }
                *o = sdscatlen(*o, &(char){'\n'}, 1);

                if (r == nhead - 1 && nhead > 0 &&
                    r < rows->len - 1) {
                    *o = sdscat(*o, ANSI_DIM);
                    *o = sdscat(*o, "├");
                    for (unsigned int c = 0; c < ncols; c++) {
                        for (int i = 0; i < widths[c] + 2; i++)
                            *o = sdscat(*o, "─");
                        if (c < ncols - 1) *o = sdscat(*o, "┼");
                    }
                    *o = sdscat(*o, "┤\n" ANSI_RESET);
                }
            }

            *o = sdscat(*o, ANSI_DIM);
            *o = sdscat(*o, "└");
            for (unsigned int c = 0; c < ncols; c++) {
                for (int i = 0; i < widths[c] + 2; i++)
                    *o = sdscat(*o, "─");
                if (c < ncols - 1) *o = sdscat(*o, "┴");
            }
            *o = sdscat(*o, "┘" ANSI_RESET "\n");

            free(widths);
            ctx->has_content = TRUE;
        }

        if (ctx->table_rows) {
            for (unsigned int i = 0; i < ctx->table_rows->len; i++) {
                struct edc_ptr_array *row = edc_ptr_array_index(ctx->table_rows, i);
                for (unsigned int j = 0; j < row->len; j++) {
                    sds cell = edc_ptr_array_index(row, j);
                    sdsfree(cell);
                }
                edc_ptr_array_free(row);
            }
            edc_ptr_array_free(ctx->table_rows);
            ctx->table_rows = NULL;
        }
        if (ctx->col_aligns) {
            for (unsigned int i = 0; i < ctx->col_aligns->len; i++) {
                int *a = edc_ptr_array_index(ctx->col_aligns, i);
                free(a);
            }
            edc_ptr_array_free(ctx->col_aligns);
            ctx->col_aligns = NULL;
        }
        break;
    }

    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        break;

    case MD_BLOCK_TR:
        if (ctx->current_row) {
            edc_ptr_array_add(ctx->table_rows, ctx->current_row);
            ctx->current_row = NULL;
        }
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (ctx->current_cell && ctx->current_row) {
            edc_ptr_array_add(ctx->current_row, ctx->current_cell);
            ctx->current_cell = NULL;
        }
        if (type == MD_BLOCK_TH && ctx->col_aligns) {
            MD_BLOCK_TD_DETAIL *td = detail;
            while (ctx->col_aligns->len <= (unsigned int)ctx->td_col_idx) {
                int *a = EDC_NEW0(int, 1);
                *a = MD_ALIGN_DEFAULT;
                edc_ptr_array_add(ctx->col_aligns, a);
            }
            int *a = edc_ptr_array_index(ctx->col_aligns, ctx->td_col_idx);
            *a = td->align;
        }
        ctx->td_col_idx++;
        break;

    default:
        break;
    }

    return 0;
}

static int edc_md_enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    struct edc_md_render_ctx *ctx = userdata;
    sds *o = (ctx->in_table && ctx->current_cell) ? &ctx->current_cell : &ctx->out;

    switch (type) {
    case MD_SPAN_EM:
        *o = sdscat(*o, ANSI_ITALIC);
        break;

    case MD_SPAN_STRONG:
        *o = sdscat(*o, ANSI_BOLD);
        break;

    case MD_SPAN_A:
        ctx->in_span_a = TRUE;
        *o = sdscat(*o, ANSI_BLUE ANSI_UNDERLINE);
        break;

    case MD_SPAN_IMG:
        break;

    case MD_SPAN_CODE:
        *o = sdscat(*o, ANSI_GRAY);
        break;

    case MD_SPAN_DEL:
        *o = sdscat(*o, ANSI_STRIKE);
        break;

    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY:
        *o = sdscat(*o, ANSI_GRAY);
        break;

    case MD_SPAN_WIKILINK:
    case MD_SPAN_U:
        *o = sdscat(*o, ANSI_UNDERLINE);
        break;

    default:
        break;
    }

    return 0;
}

static int edc_md_leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    struct edc_md_render_ctx *ctx = userdata;
    sds *o = (ctx->in_table && ctx->current_cell) ? &ctx->current_cell : &ctx->out;

    switch (type) {
    case MD_SPAN_EM:
    case MD_SPAN_STRONG:
        *o = sdscat(*o, ANSI_RESET);
        break;

    case MD_SPAN_A: {
        ctx->in_span_a = FALSE;
        *o = sdscat(*o, ANSI_RESET);
        MD_SPAN_A_DETAIL *det = detail;
        if (!det->is_autolink && det->href.text && det->href.size > 0) {
            *o = sdscat(*o, " " ANSI_BLUE "(");
            *o = sdscatlen(*o, det->href.text, det->href.size);
            *o = sdscat(*o, ")" ANSI_RESET);
        }
        break;
    }

    case MD_SPAN_IMG: {
        *o = sdscat(*o, ANSI_BLUE "[img");
        MD_SPAN_IMG_DETAIL *det = detail;
        if (det->title.text && det->title.size > 0) {
            *o = sdscatlen(*o, &(char){':'}, 1);
            *o = sdscatlen(*o, det->title.text, det->title.size);
        }
        *o = sdscat(*o, "]");
        if (det->src.text && det->src.size > 0) {
            *o = sdscatlen(*o, det->src.text, det->src.size);
        }
        *o = sdscat(*o, ANSI_RESET);
        break;
    }

    case MD_SPAN_CODE:
    case MD_SPAN_DEL:
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY:
        *o = sdscat(*o, ANSI_RESET);
        break;

    case MD_SPAN_WIKILINK: {
        MD_SPAN_WIKILINK_DETAIL *det = detail;
        if (det->target.text && det->target.size > 0) {
            *o = sdscat(*o, ANSI_BLUE " (");
            *o = sdscatlen(*o, det->target.text, det->target.size);
            *o = sdscat(*o, ")" ANSI_RESET);
        }
        break;
    }

    case MD_SPAN_U:
        *o = sdscat(*o, ANSI_RESET);
        break;

    default:
        break;
    }

    return 0;
}

static int edc_md_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size,
                       void *userdata)
{
    struct edc_md_render_ctx *ctx = userdata;
    sds *o = (ctx->in_table && ctx->current_cell) ? &ctx->current_cell : &ctx->out;

    switch (type) {
    case MD_TEXT_NORMAL:
        *o = sdscatlen(*o, text, size);
        break;

    case MD_TEXT_NULLCHAR:
        break;

    case MD_TEXT_BR:
        if (ctx->in_table && ctx->current_cell) {
            *o = sdscatlen(*o, &(char){' '}, 1);
        } else {
            *o = sdscatlen(*o, &(char){'\n'}, 1);
            if (ctx->in_quote)
                *o = sdscat(*o, ANSI_DIM "│ " ANSI_RESET);
            else if (ctx->list_depth > 0) {
                int depth = ctx->list_depth - 1;
                if (depth >= MAX_LIST_DEPTH) depth = MAX_LIST_DEPTH - 1;
                if (depth < 0) depth = 0;
                *o = sdscatprintf(*o, "%*s", depth * 2 + 2, "");
            }
        }
        break;

    case MD_TEXT_SOFTBR:
        if (ctx->in_table && ctx->current_cell) {
            *o = sdscatlen(*o, &(char){' '}, 1);
        } else {
            *o = sdscatlen(*o, &(char){' '}, 1);
        }
        break;

    case MD_TEXT_CODE:
    case MD_TEXT_HTML:
    case MD_TEXT_LATEXMATH:
        *o = sdscatlen(*o, text, size);
        break;

    case MD_TEXT_ENTITY:
        *o = sdscatlen(*o, text, size);
        break;

    default:
        break;
    }

    ctx->has_content = TRUE;
    return 0;
}

static void edc_md_debug_log(const char *msg, void *userdata)
{
    (void)userdata;
    log_debug("md4c: %s", msg);
}

sds edc_markdown_render(sds markdown)
{
    if (!markdown) return sdsempty();

    struct edc_md_render_ctx ctx;
    edc_md_render_ctx_init(&ctx);

    MD_PARSER parser = {
        .abi_version = 0,
        .flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH |
                 MD_FLAG_TASKLISTS | MD_FLAG_UNDERLINE |
                 MD_FLAG_WIKILINKS |
                 MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_COLLAPSEWHITESPACE,
        .enter_block = edc_md_enter_block,
        .leave_block = edc_md_leave_block,
        .enter_span = edc_md_enter_span,
        .leave_span = edc_md_leave_span,
        .text = edc_md_text,
        .debug_log = edc_md_debug_log,
        .syntax = NULL,
    };

    int ret = md_parse(markdown, (MD_SIZE)sdslen(markdown), &parser, &ctx);
    if (ret != 0) {
        log_error("md_parse failed: %d", ret);
    }

    sds result = ctx.out;
    ctx.out = NULL;
    edc_md_render_ctx_free(&ctx);

    return result;
}

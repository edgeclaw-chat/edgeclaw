/* `/command` 行现在也进文法（S2）：词法器（SHELL_OFF）→ edc_parse → EDC_AST_COMMAND。

 * 钉住：
 *   ① 名字来自 TK_SLASH_CMD，参数是后续 words；
 *   ② 参数已由词法器去引号（`'D:\my repo'` 是一个词）；
 *   ③ 命令行里算符与选项都是普通参数（`|`、`--bg` 不被当语法）；
 *   ④ 未闭合引号 = 解析失败 + 可读原因；
 *   ⑤ 同一条文法仍在解析 shell 行（EDC_AST_SHELL），两条入口互不干扰。 */
#include "edc_ast.h"
#include "edc_lex.h"
#include "edc_parse.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* 一条命令行的解析结果；失败时返回 false（并已填 *err）。 */
static bool parse_line(const char *text, edc_ast *ast, sds *err)
{
    edc_lex_out toks;
    memset(ast, 0, sizeof(*ast));
    *err = NULL;
    if (!edc_lex(text, EDC_LEX_SHELL_OFF, &toks))
        return false;
    const bool ok = edc_parse(&toks, ast, err);
    edc_lex_out_free(&toks);
    return ok;
}

/* The shell-line counterpart of parse_line(): same lex + edc_parse, but with the switch
 * ON -- which is the only mode where T_OPT/T_OPTARG exist at all (SHELL_OFF folds them
 * into ordinary words, on purpose: that is what a command line wants). */
static bool parse_shell_line(const char *text, edc_ast *ast, sds *err)
{
    edc_lex_out toks;
    memset(ast, 0, sizeof(*ast));
    *err = NULL;
    if (!edc_lex(text, EDC_LEX_SHELL_ON, &toks))
        return false;
    const bool ok = edc_parse(&toks, ast, err);
    edc_lex_out_free(&toks);
    return ok;
}

int main(void)
{
    edc_ast ast;
    sds err = NULL;

    /* ── ① 名字 + 参数 ─────────────────────────────────────── */
    TEST("/session new dev: name + two words");
    CHECK(parse_line("/session new dev", &ast, &err), "parsed");
    CHECK(ast.kind == EDC_AST_COMMAND, "kind = COMMAND");
    CHECK(ast.cmd && ast.cmd->name && strcmp(ast.cmd->name, "session") == 0, "name without the slash");
    CHECK(ast.cmd && ast.cmd->argc == 2, "two arguments");
    CHECK(ast.cmd && strcmp(ast.cmd->argv[0], "new") == 0 && strcmp(ast.cmd->argv[1], "dev") == 0, "argv values");
    edc_ast_free(&ast);

    TEST("/session alone: name, no arguments");
    CHECK(parse_line("/session", &ast, &err), "parsed");
    CHECK(ast.kind == EDC_AST_COMMAND && ast.cmd && ast.cmd->argc == 0, "argc == 0");
    CHECK(ast.cmd && strcmp(ast.cmd->name, "session") == 0, "name survives");
    edc_ast_free(&ast);

    /* ── ② 引号已由词法器处理 ───────────────────────────────── */
    TEST("/sandbox add-rw 'D:\\my repo': the quoted path is ONE argument");
    CHECK(parse_line("/sandbox add-rw 'D:\\my repo'", &ast, &err), "parsed");
    CHECK(ast.cmd && ast.cmd->argc == 2, "verb + one path");
    CHECK(ast.cmd && strcmp(ast.cmd->argv[1], "D:\\my repo") == 0, "quotes are syntax");
    edc_ast_free(&ast);

    /* ── ③ 命令行里算符/选项都是普通参数 ───────────────────── */
    TEST("/session memo search a|b: the pipe is an argument");
    CHECK(parse_line("/session memo search a|b", &ast, &err), "parsed");
    CHECK(ast.cmd && ast.cmd->argc == 3, "three arguments");
    CHECK(ast.cmd && strcmp(ast.cmd->argv[2], "a|b") == 0, "a|b is one word");
    edc_ast_free(&ast);

    /* A command line has no "option area" to catch a second operand, so two adjacent
     * quoted regions would silently become two argv entries -- a different command call,
     * not a syntax error. edc_commands_parse_line refuses it before parse; this pins the
     * lexer-level fact it keys on (`adjacent_quoted`), because that is what makes the
     * refusal possible at all. */
    TEST("two adjacent quoted regions are flagged, so the command layer can refuse them");
    {
        edc_lex_out t;
        CHECK(edc_lex("/task new \"a\"\"b\"", EDC_LEX_SHELL_OFF, &t), "lex ok (SHELL_OFF, as commands use)");
        CHECK(t.adjacent_quoted, "flagged even with the switch off");
        CHECK(t.count >= 3, "and the two regions are separate words");
        edc_lex_out_free(&t);

        CHECK(edc_lex("/task new \"a\" 'b'", EDC_LEX_SHELL_OFF, &t), "lex ok");
        CHECK(!t.adjacent_quoted, "a space between them is not adjacency");
        edc_lex_out_free(&t);
    }

    TEST("/model --bg: an option word is an ordinary argument");
    CHECK(parse_line("/model --bg", &ast, &err), "parsed");
    CHECK(ast.cmd && ast.cmd->argc == 1 && strcmp(ast.cmd->argv[0], "--bg") == 0, "--bg kept as a word");
    edc_ast_free(&ast);

    /* ── ③b 带值选项也必须各自成词（这条守的是 PRODUCTION 路径）─────
     *
     * Regression, and it belongs HERE rather than only in test_lex: the lexer's option
     * rules used to match their trailing space as part of the option, so no C_WS followed
     * to end the word being built and SHELL_OFF folded the option into it -- `-d t1`
     * arrived as ONE word `-dt1`. Every command handler's getopt reads these words, so
     * this exact property is what makes `/task new x -d 1` mean anything.
     *
     * Asserted through lex + edc_parse (what the dispatcher calls), not through a helper. */
    TEST("/task new x -d 1: `-d` and its value are separate words");
    CHECK(parse_line("/task new x -d 1", &ast, &err), "parsed");
    CHECK(ast.cmd && ast.cmd->argc == 4, "four words after the name");
    CHECK(ast.cmd && strcmp(ast.cmd->argv[2], "-d") == 0, "`-d` is its own word");
    CHECK(ast.cmd && strcmp(ast.cmd->argv[3], "1") == 0, "and the value is its own word");
    edc_ast_free(&ast);

    TEST("/task new x --group g1: same for a long option");
    CHECK(parse_line("/task new x --group g1", &ast, &err), "parsed");
    CHECK(ast.cmd && ast.cmd->argc == 4 && strcmp(ast.cmd->argv[2], "--group") == 0 &&
              strcmp(ast.cmd->argv[3], "g1") == 0,
          "`--group` and `g1` are separate words");
    edc_ast_free(&ast);

    TEST("/task new 'x -d 1': quoting makes the whole thing ONE argument");
    CHECK(parse_line("/task new 'x -d 1'", &ast, &err), "parsed");
    CHECK(ast.cmd && ast.cmd->argc == 2, "name + one quoted word");
    CHECK(ast.cmd && strcmp(ast.cmd->argv[1], "x -d 1") == 0, "the option-looking text stayed inside");
    CHECK(ast.cmd && ast.cmd->argv_quoted && ast.cmd->argv_quoted[1] && !ast.cmd->argv_quoted[0],
          "argv_quoted: the quoted word is flagged, nothing else is");
    edc_ast_free(&ast);

    /* ── ④ 未闭合引号 ──────────────────────────────────────── */
    TEST("/session new dev 'oops: parse fails with a reason");
    CHECK(!parse_line("/session new dev 'oops", &ast, &err), "parse rejected");
    CHECK(err != NULL && strstr(err, "Unterminated") != NULL, "the error names the problem");
    sdsfree(err);
    edc_ast_free(&ast);

    /* ── ⑤ shell 行仍走同一条文法（两条入口互不干扰） ─────── */
    TEST("\"a\" | \"b\": still a SHELL line, no command node");
    {
        edc_lex_out toks;
        CHECK(edc_lex("\"a\" | \"b\"", EDC_LEX_SHELL_ON, &toks), "lex ok");
        sds perr = NULL;
        const bool ok = edc_parse(&toks, &ast, &perr);
        edc_lex_out_free(&toks);
        if (!ok)
            printf("[parse error: %s] ", perr ? perr : "?");
        CHECK(ok, "parsed");
        CHECK(ast.kind == EDC_AST_SHELL, "kind = SHELL (the command rule did not swallow it)");
        CHECK(ast.cmd == NULL, "no command node");
        CHECK(ast.pipelines != NULL && ast.pipelines->stages && ast.pipelines->stages->next != NULL,
              "two stages in one pipeline");
        sdsfree(perr);
        edc_ast_free(&ast);
    }

    /* ── ⑥ shell 行的选项：有参的四个，缺值时给的是"少了东西"而不是 bison 的原文 ──
     *
     * 两条独立的事实，都曾经不对：
     *   ① 短选项的尾随上下文是 `[ \t]`、长选项是 `[ \t\n]`，于是行尾的 `-d` 掉回普通
     *      WORD，和 `--group` 走了两条完全不同的失败路径；
     *   ② 两种路径都直接把 bison 的 "syntax error, unexpected …" 抛给用户。
     * 现在四个选项在行尾都成 OPTARG，且缺值走同一条可读文案（预检里，见 edc_parse）。 */
    TEST("shell line: an option with a value keeps it, even at end of line");
    {
        edc_lex_out toks;
        CHECK(edc_lex("\"a\" -d", EDC_LEX_SHELL_ON, &toks), "lex ok");
        CHECK(toks.count >= 3 && toks.toks[1].kind == TK_OPTARG && toks.toks[1].value &&
                  strcmp(toks.toks[1].value, "-d") == 0,
              "a trailing `-d` is an OPTARG (it used to fall back to a plain WORD)");
        edc_lex_out_free(&toks);
    }

    TEST("shell line: a missing option value is refused with a readable message");
    {
        static const struct
        {
            const char *line;
            const char *option; /* the name that has to appear in the reason */
        } cases[] = {{"\"a\" -d", "-d"},
                     {"\"a\" -g", "-g"},
                     {"\"a\" --with", "--with"},
                     {"\"a\" --group", "--group"}};
        for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++)
        {
            edc_ast a;
            sds perr = NULL;
            const bool ok = parse_shell_line(cases[ci].line, &a, &perr);
            char what[128];
            snprintf(what, sizeof(what), "%s is refused", cases[ci].line);
            CHECK(!ok, what);
            snprintf(what, sizeof(what), "%s: the reason says the option needs a value", cases[ci].line);
            CHECK(perr != NULL && strstr(perr, "needs a value") != NULL, what);
            snprintf(what, sizeof(what), "%s: it names `%s`", cases[ci].line, cases[ci].option);
            CHECK(perr != NULL && strstr(perr, cases[ci].option) != NULL, what);
            sdsfree(perr);
            edc_ast_free(&a);
        }
    }

    TEST("shell line: a quoted value after an option is still one word");
    {
        edc_ast a;
        sds perr = NULL;
        CHECK(parse_shell_line("\"a\" -d \"t 1\"", &a, &perr), "parsed");
        CHECK(a.pipelines && a.pipelines->stages && a.pipelines->stages->ndep == 1, "one dependency");
        CHECK(a.pipelines && a.pipelines->stages && a.pipelines->stages->deps &&
                  strcmp(a.pipelines->stages->deps[0], "t 1") == 0,
              "the quoted value kept its space");
        edc_ast_free(&a);
        sdsfree(perr);
    }

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

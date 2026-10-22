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

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

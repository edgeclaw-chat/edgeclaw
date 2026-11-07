/* 新词法器（flex）的判据 —— 第①步。
 *   `!git log | head` → SIGIL_LOCALSHELL + 一个 TK_RAW_REST（里面的 | 不是算符）
 *   `a"b c"d`         → 三个词：TEXT`a` / STRING`b c` / TEXT`d`（不混拼）
 *   `"a" 2>> "e"`     → REDIR{fd=2, append=true}
 * 这一步**不改生产行为**（老 tokenizer 仍在用），所以这里只验新 API。 */
#include "edc_lex.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Split a tail the way a shell does -- a TEST helper only.
 *
 * This used to be `edc_argv_from_tail` in edc_lex.h. Production does not need it: the
 * dispatcher sends a command line through edc_lex + edc_parse (the grammar builds
 * `struct edc_ast_cmd`), and the lexer is the only place that decides word boundaries.
 * Keeping a public "split this raw text into argv" call would invite the next handler to
 * bypass the grammar -- which is the very dialect this refactor removed -- so it lives
 * here, on top of the real lexer, and nothing can call it from src/.
 *
 * Quotes are syntax (adjacent fragments join, quotes are gone); operators do not
 * participate (SHELL_OFF). An unterminated quote is an ERROR: NULL plus a readable
 * message in |out_error|, never a silently dropped argument. Free with
 * sdsfreesplitres(). */
static sds *edc_argv_from_tail(const char *tail, int *out_count, sds *out_error)
{
    if (out_count)
        *out_count = 0;
    if (out_error)
        *out_error = NULL;

    edc_lex_out o;
    if (!edc_lex(tail ? tail : "", EDC_LEX_SHELL_OFF, &o))
        return NULL;

    sds *args = NULL;
    int n = 0;
    for (int i = 0; i < o.count; i++)
    {
        if (o.toks[i].kind == TK_BAD_QUOTED)
        {
            if (out_error)
                *out_error = edc_lex_unterminated_quote(o.toks[i].value, o.toks[i].begin + 1);
            edc_lex_out_free(&o);
            sdsfreesplitres(args, n);
            return NULL;
        }
        if (o.toks[i].kind != TK_TEXT && o.toks[i].kind != TK_STRING)
            continue;
        args = realloc(args, sizeof(sds) * (size_t)(n + 1));
        args[n++] = sdsdup(o.toks[i].value ? o.toks[i].value : sdsempty());
    }
    edc_lex_out_free(&o);
    if (out_count)
        *out_count = n;
    return args;
}

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-56s ", n);                                                                                   \
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

int main(void)
{
    edc_lex_out o;

    TEST("`!git log | head` : the payload is ONE token, the `|` is not an operator");
    CHECK(edc_lex("!git log | head", EDC_LEX_SHELL_ON, &o), "the lexer never fails");
    CHECK(o.count == 3, "SIGIL + RAW_REST + EOF");
    CHECK(o.toks[0].kind == TK_SIGIL_LOCALSHELL, "it starts with the local-shell sigil");
    CHECK(o.toks[1].kind == TK_RAW_REST, "then one raw-rest token");
    CHECK(o.toks[1].value && strcmp(o.toks[1].value, "git log | head") == 0, "carrying the whole command line");
    edc_lex_out_free(&o);

    /* Quoted and unquoted text are NOT concatenated (decided 2026-10-22): a word is one
     * TEXT or one STRING, so `a"b c"d` is three words. C concatenates adjacent literals;
     * we do not, and this is the test that says so. */
    TEST("`a\"b c\"d` : quoted and bare text do NOT mix -- three words");
    CHECK(edc_lex("a\"b c\"d", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 4, "three words + EOF");
    CHECK(o.toks[0].kind == TK_TEXT && o.toks[0].value && strcmp(o.toks[0].value, "a") == 0, "bare `a`");
    CHECK(o.toks[1].kind == TK_STRING && o.toks[1].value && strcmp(o.toks[1].value, "b c") == 0, "quoted `b c`");
    CHECK(o.toks[2].kind == TK_TEXT && o.toks[2].value && strcmp(o.toks[2].value, "d") == 0, "bare `d`");
    edc_lex_out_free(&o);

    TEST("`\"a b\"` : one STRING (that is what makes it a shell line)");
    CHECK(edc_lex("\"a b\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 2 && o.toks[0].kind == TK_STRING, "one word + EOF");
    CHECK(o.toks[0].value && strcmp(o.toks[0].value, "a b") == 0, "quotes are delimiters");
    CHECK(o.mode == EDC_LEX_SHELL_ON, "and the stream remembers which mode cut it");
    edc_lex_out_free(&o);

    /* The switch-off counterpart: still a STRING lexically, but quoting is NOT syntax
     * there -- which is why the fact and the switch are two different fields. */
    TEST("`\"a b\"` with the switch OFF is still a STRING, but the mode says OFF");
    CHECK(edc_lex("\"a b\"", EDC_LEX_SHELL_OFF, &o), "lex ok");
    CHECK(o.count == 2 && o.toks[0].kind == TK_STRING, "lexically the same token");
    CHECK(o.mode == EDC_LEX_SHELL_OFF, "the mode is what makes it NOT a shell line");
    edc_lex_out_free(&o);

    TEST("`\"a\" 2>> \"e\"` : the redirect carries fd=2 and append");
    CHECK(edc_lex("\"a\" 2>> \"e\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 4, "word + redir + word + EOF");
    CHECK(o.toks[1].kind == TK_REDIR && o.toks[1].fd == 2 && o.toks[1].append, "fd 2, append (the `2>>` bug)");
    edc_lex_out_free(&o);

    TEST("`&` at the end is BG; `\"a\" & x` is not");
    CHECK(edc_lex("&", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 2 && o.toks[0].kind == TK_BG, "lone `&` -> BG");
    edc_lex_out_free(&o);
    CHECK(edc_lex("\"a\" & x", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 4, "three words (a, &, x) + EOF -- no BG token");
    CHECK(o.toks[0].kind == TK_STRING && o.toks[1].kind == TK_TEXT && o.toks[2].kind == TK_TEXT,
          "`a` is a STRING, the `&` and `x` are TEXT");
    edc_lex_out_free(&o);

    TEST("`&&` is ONE operator token, not two `&` words");
    CHECK(edc_lex("\"a\" && \"b\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 4, "word + && + word + EOF");
    CHECK(o.toks[0].kind == TK_STRING && o.toks[1].kind == TK_ANDAND && o.toks[2].kind == TK_STRING,
          "the middle token is ANDAND");
    edc_lex_out_free(&o);

    TEST("`&&` at the end of the line is not the background `&`");
    CHECK(edc_lex("\"a\" &&", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 3 && o.toks[1].kind == TK_ANDAND, "still ANDAND (BG is a lone `&` at EOL)");
    edc_lex_out_free(&o);

    TEST("`||` is ONE operator token, and `|` still reads as a pipe");
    CHECK(edc_lex("\"a\" || \"b\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 4, "word + || + word + EOF");
    CHECK(o.toks[1].kind == TK_OROR, "the middle token is OROR");
    edc_lex_out_free(&o);
    CHECK(edc_lex("\"a\" | \"b\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 4 && o.toks[1].kind == TK_PIPE, "a single `|` is still a PIPE");
    edc_lex_out_free(&o);

    TEST("`--bg` is an OPTION; `-dfoo` / `--bg=1` are ordinary words");
    CHECK(edc_lex("--bg", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 2 && o.toks[0].kind == TK_OPT, "at end of line it is still an option");
    edc_lex_out_free(&o);
    CHECK(edc_lex("-dfoo", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 2 && o.toks[0].kind == TK_TEXT, "no attached form -- it is plain text");
    edc_lex_out_free(&o);

    TEST("`+foo` : sigil, then a plain word (which the parser will call a message)");
    CHECK(edc_lex("+foo", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 3 && o.toks[0].kind == TK_SIGIL_NEWGROUP && o.toks[1].kind == TK_TEXT, "sigil + text + EOF");
    edc_lex_out_free(&o);

    TEST("`{{LAST}}` is marked only inside double quotes");
    CHECK(edc_lex("\"see {{LAST}}\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.toks[0].kind == TK_STRING && o.toks[0].has_var_refs, "double quotes: marked");
    edc_lex_out_free(&o);
    CHECK(edc_lex("'see {{LAST}}'", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.toks[0].kind == TK_STRING && !o.toks[0].has_var_refs, "single quotes: not marked");
    edc_lex_out_free(&o);

    TEST("switch OFF: no operator tokens, and the fact is recorded for the note");
    CHECK(edc_lex("\"a\" | \"b\"", EDC_LEX_SHELL_OFF, &o), "lex ok");
    CHECK(o.saw_disabled, "the `|` was seen and refused");
    CHECK(o.toks[0].kind == TK_STRING, "the leading quoted word is still a STRING");
    CHECK(o.mode == EDC_LEX_SHELL_OFF, "and the stream says the switch was OFF");
    edc_lex_out_free(&o);

    TEST("an unclosed quote is one token, and the error names it AND where it starts");
    CHECK(edc_lex("\"a\" ; \"unclosed tail", EDC_LEX_SHELL_ON, &o), "lex ok");
    {
        int bad = -1;
        for (int i = 0; i < o.count; i++)
            if (o.toks[i].kind == TK_BAD_QUOTED)
                bad = i;
        CHECK(bad >= 0, "the lexer marks the bad token itself (no side channel)");
        if (bad >= 0)
        {
            sds msg = edc_lex_unterminated_quote(o.toks[bad].value, o.toks[bad].begin + 1);
            /* The three call sites (parser / `+` branch / classifier) all use this
             * one builder, so pinning its shape here pins all of them. */
            CHECK(strstr(msg, "at column 7") != NULL, "the column is where the quote opens");
            CHECK(strstr(msg, "unclosed tail") != NULL, "and the offending text is quoted back");
            CHECK(strstr(msg, "`\"text\"`") != NULL, "with the shape of a correct line");
            sdsfree(msg);
        }
    }
    edc_lex_out_free(&o);

    TEST("a very long unclosed quote is truncated, not dumped in full");
    {
        sds msg = edc_lex_unterminated_quote("\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
        CHECK(sdslen(msg) < 160, "the snippet is capped");
        CHECK(strstr(msg, "…") != NULL, "and says it was cut");
        sdsfree(msg);
    }

    /* ── 命令参数：edc_argv_from_tail 走同一套 bash 词法 ─────────────────── */
    TEST("argv: quotes are syntax, backslashes are bytes");
    {
        int n = 0;
        sds err = NULL;
        sds *argv = edc_argv_from_tail("dev 'D:\\repo\\sub'", &n, &err);
        CHECK(argv != NULL && n == 2, "two words");
        CHECK(argv && strcmp(argv[0], "dev") == 0, "the bare word");
        CHECK(argv && strcmp(argv[1], "D:\\repo\\sub") == 0, "the quoted Windows path, quotes gone");
        CHECK(err == NULL, "no error");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("D:\\github\\edgeclaw", &n, &err);
        CHECK(argv != NULL && n == 1 && strcmp(argv[0], "D:\\github\\edgeclaw") == 0, "unquoted path keeps every backslash");
        sdsfreesplitres(argv, n);
    }

    TEST("argv: `\\n` in double quotes is literal, $'…' is where it escapes");
    {
        int n = 0;
        sds err = NULL;
        sds *argv = edc_argv_from_tail("\"a\\nb\"", &n, &err);
        CHECK(argv != NULL && n == 1 && strcmp(argv[0], "a\\nb") == 0, "double quotes: backslash + n");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("$'a\\nb'", &n, &err);
        CHECK(argv != NULL && n == 1 && strcmp(argv[0], "a\nb") == 0, "$'…': a real newline");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("'''a\\nb'''", &n, &err);
        CHECK(argv != NULL && n == 1 && strcmp(argv[0], "a\nb") == 0, "'''…''': the same, in-tree alias");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("'a\\nb'", &n, &err);
        CHECK(argv != NULL && n == 1 && strcmp(argv[0], "a\\nb") == 0, "single quotes stay raw");
        sdsfreesplitres(argv, n);
    }

    TEST("argv: a quoted argument with spaces is ONE word");
    {
        int n = 0;
        sds err = NULL;
        sds *argv = edc_argv_from_tail("spaced 'my repo' tail", &n, &err);
        CHECK(argv != NULL && n == 3, "three words");
        CHECK(argv && strcmp(argv[1], "my repo") == 0, "the middle one kept its space");
        sdsfreesplitres(argv, n);
    }

    /* The whole reason the command layer takes argv at all: an option is an option only
     * when it is its OWN word, and quoting is what decides that. If the splitter glued an
     * option to its value (`-dt1`), no handler could tell these two lines apart. */
    TEST("argv: quoting is what separates an option's value from the description");
    {
        int n = 0;
        sds err = NULL;
        /* Unquoted: the option and its value are separate words -- the handler's getopt
         * sees `-d 1`, and the description is the one non-option word. */
        sds *argv = edc_argv_from_tail("test-x -d 1", &n, &err);
        CHECK(argv != NULL && n == 3, "three words");
        CHECK(argv && strcmp(argv[0], "test-x") == 0, "the description");
        CHECK(argv && strcmp(argv[1], "-d") == 0, "`-d` is its own word");
        CHECK(argv && strcmp(argv[2], "1") == 0, "and its value is too");
        sdsfreesplitres(argv, n);

        /* Quoted: ONE word that happens to contain an option-looking text. */
        argv = edc_argv_from_tail("'test-x -d 1'", &n, &err);
        CHECK(argv != NULL && n == 1, "one word");
        CHECK(argv && strcmp(argv[0], "test-x -d 1") == 0, "the whole text is the description");
        sdsfreesplitres(argv, n);
    }

    TEST("argv: `-d` / `-g` / `--with` / `--group` never swallow their value");
    {
        int n = 0;
        sds err = NULL;
        /* Regression: these rules used to match the trailing space as part of the option,
         * so no C_WS followed and SHELL_OFF folded the option into the word being built --
         * `-d t1` arrived as ONE word `-dt1`. See edc_lex.l's option rules. */
        sds *argv = edc_argv_from_tail("x -d t1", &n, &err);
        CHECK(argv != NULL && n == 3 && strcmp(argv[1], "-d") == 0 && strcmp(argv[2], "t1") == 0,
              "`-d t1` is two words");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("x -g g1", &n, &err);
        CHECK(argv != NULL && n == 3 && strcmp(argv[1], "-g") == 0 && strcmp(argv[2], "g1") == 0,
              "`-g g1` is two words");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("x --with t1", &n, &err);
        CHECK(argv != NULL && n == 3 && strcmp(argv[1], "--with") == 0 && strcmp(argv[2], "t1") == 0,
              "`--with t1` is two words");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("x --group g1", &n, &err);
        CHECK(argv != NULL && n == 3 && strcmp(argv[1], "--group") == 0 && strcmp(argv[2], "g1") == 0,
              "`--group g1` is two words");
        sdsfreesplitres(argv, n);

        /* Several in a row, and at the end of the line. */
        argv = edc_argv_from_tail("x -d t1 --bg -g g1", &n, &err);
        CHECK(argv != NULL && n == 6, "six words");
        CHECK(argv && strcmp(argv[1], "-d") == 0 && strcmp(argv[2], "t1") == 0, "first pair");
        CHECK(argv && strcmp(argv[3], "--bg") == 0 && strcmp(argv[4], "-g") == 0, "then the flags");
        CHECK(argv && strcmp(argv[5], "g1") == 0, "and the last value");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("x --bg", &n, &err);
        CHECK(argv != NULL && n == 2 && strcmp(argv[1], "--bg") == 0, "a trailing flag stands alone");
        sdsfreesplitres(argv, n);

        argv = edc_argv_from_tail("x --new-group", &n, &err);
        CHECK(argv != NULL && n == 2 && strcmp(argv[1], "--new-group") == 0, "so does --new-group");
        sdsfreesplitres(argv, n);
    }

    TEST("argv: `-dfoo` is a word, not an option with a glued value");
    {
        int n = 0;
        sds err = NULL;
        /* Separated form only: a value must be its own word. */
        sds *argv = edc_argv_from_tail("x -dt1", &n, &err);
        CHECK(argv != NULL && n == 2 && strcmp(argv[1], "-dt1") == 0, "one word `-dt1`");
        sdsfreesplitres(argv, n);
    }

    TEST("argv: an unterminated quote is an ERROR, not a dropped argument");
    {
        int n = 0;
        sds err = NULL;
        sds *argv = edc_argv_from_tail("dev 'oops", &n, &err);
        CHECK(argv == NULL, "no argv");
        CHECK(n == 0, "no words");
        CHECK(err != NULL && strstr(err, "Unterminated") != NULL, "the error names the problem");
        sdsfree(err);

        err = NULL;
        argv = edc_argv_from_tail("dev \"oops", &n, &err);
        CHECK(argv == NULL && err != NULL, "same for a double quote");
        sdsfree(err);
    }

    /* The triple bodies follow PYTHON's rule: a run of ONE or TWO delimiters inside the
     * body is literal text and only a run of three closes the block. The lexer used to
     * spell the body `[^"]*` / `[^']*`, which excluded exactly that -- so the block failed
     * to match as a whole, fell through to the ORDINARY quote rule (longest match takes
     * `"…"` for the first two quotes), and ONE `"""…"""` reached the parser as
     * STRING/TEXT/STRING. On a shell line that is `syntax error, unexpected T_TEXT,
     * expecting end of file`; on `/task new` the description silently became three argv
     * words. These three cases pin the lexer half; //tests:test_cmd_parse pins the
     * unquoting half (edc_cmd_unquote already agreed with Python). */
    TEST("`\"\"\"` body holds a bare quote -- ONE STRING token, not STRING/TEXT/STRING");
    {
        CHECK(edc_lex("\"\"\"he said \"hi\" here\"\"\"", EDC_LEX_SHELL_ON, &o), "lex ok");
        CHECK(o.count == 2, "ONE word + EOF");
        CHECK(o.toks[0].kind == TK_STRING, "and that word is a STRING");
        CHECK(o.toks[0].value && strcmp(o.toks[0].value, "he said \"hi\" here") == 0, "the inner quotes survive in the value");
        edc_lex_out_free(&o);
    }

    TEST("`'''` body holds a bare apostrophe -- ONE STRING token");
    {
        CHECK(edc_lex("'''it's fine'''", EDC_LEX_SHELL_ON, &o), "lex ok");
        CHECK(o.count == 2 && o.toks[0].kind == TK_STRING, "ONE STRING + EOF");
        CHECK(o.toks[0].value && strcmp(o.toks[0].value, "it's fine") == 0, "the apostrophe survives");
        edc_lex_out_free(&o);
    }

    TEST("the same block on a command line (SHELL_OFF) is still ONE word");
    {
        CHECK(edc_lex("/task new \"\"\"he said \"hi\" here\"\"\"", EDC_LEX_SHELL_OFF, &o), "lex ok");
        CHECK(o.count == 4, "SLASH + `new` + ONE STRING + EOF");
        CHECK(o.toks[2].kind == TK_STRING && o.toks[2].value &&
              strcmp(o.toks[2].value, "he said \"hi\" here") == 0,
              "the description is one word, not three");
        edc_lex_out_free(&o);
    }

    TEST("an unterminated `\"\"\"` block with a bare quote is ONE bad token");
    {
        CHECK(edc_lex("\"\"\"a \"b\" c", EDC_LEX_SHELL_ON, &o), "lex ok");
        CHECK(o.count == 2 && o.toks[0].kind == TK_BAD_QUOTED,
              "one BADQ token (the unterminated rule got the same body), not three words");
        edc_lex_out_free(&o);
    }

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

/* 新词法器（flex）的判据 —— 第①步。
 *   `!git log | head` → SIGIL_LOCALSHELL + 一个 TK_RAW_REST（里面的 | 不是算符）
 *   `a"b c"d`         → 一个 word「ab cd」，fully_quoted=false
 *   `"a" 2>> "e"`     → REDIR{fd=2, append=true}
 * 这一步**不改生产行为**（老 tokenizer 仍在用），所以这里只验新 API。 */
#include "edc_lex.h"

#include <stdio.h>
#include <string.h>

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

    TEST("`a\"b c\"d` : adjacent fragments are ONE word (shell word splitting)");
    CHECK(edc_lex("a\"b c\"d", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 2, "one word + EOF");
    CHECK(o.toks[0].kind == TK_WORD, "it is a word");
    CHECK(o.toks[0].value && strcmp(o.toks[0].value, "ab cd") == 0, "the fragments concatenated (got the value)");
    CHECK(!o.toks[0].fully_quoted, "and it is not fully quoted");
    edc_lex_out_free(&o);

    TEST("`\"a b\"` : one word, FULLY quoted (that is what makes it a shell line)");
    CHECK(edc_lex("\"a b\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 2 && o.toks[0].kind == TK_WORD, "one word + EOF");
    CHECK(o.toks[0].value && strcmp(o.toks[0].value, "a b") == 0, "quotes are delimiters");
    CHECK(o.toks[0].fully_quoted, "fully_quoted = true");
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
    CHECK(o.toks[0].kind == TK_WORD && o.toks[1].kind == TK_WORD && o.toks[2].kind == TK_WORD,
          "the `&` stayed a word");
    edc_lex_out_free(&o);

    TEST("`&&` is ONE operator token, not two `&` words");
    CHECK(edc_lex("\"a\" && \"b\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 4, "word + && + word + EOF");
    CHECK(o.toks[0].kind == TK_WORD && o.toks[1].kind == TK_ANDAND && o.toks[2].kind == TK_WORD,
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
    CHECK(o.count == 2 && o.toks[0].kind == TK_WORD, "no attached form");
    edc_lex_out_free(&o);

    TEST("`+foo` : sigil, then a plain word (which the parser will call a message)");
    CHECK(edc_lex("+foo", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.count == 3 && o.toks[0].kind == TK_SIGIL_NEWGROUP && o.toks[1].kind == TK_WORD, "sigil + word + EOF");
    CHECK(o.toks[1].kind == TK_WORD && !o.toks[1].fully_quoted, "and the word is not fully quoted");
    edc_lex_out_free(&o);

    TEST("`{{LAST}}` is marked only inside double quotes");
    CHECK(edc_lex("\"see {{LAST}}\"", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.toks[0].kind == TK_WORD && o.toks[0].has_var_refs, "double quotes: marked");
    edc_lex_out_free(&o);
    CHECK(edc_lex("'see {{LAST}}'", EDC_LEX_SHELL_ON, &o), "lex ok");
    CHECK(o.toks[0].kind == TK_WORD && !o.toks[0].has_var_refs, "single quotes: not marked");
    edc_lex_out_free(&o);

    TEST("switch OFF: no operator tokens, and the fact is recorded for the note");
    CHECK(edc_lex("\"a\" | \"b\"", EDC_LEX_SHELL_OFF, &o), "lex ok");
    CHECK(o.saw_disabled, "the `|` was seen and refused");
    CHECK(o.toks[0].kind == TK_WORD, "everything is words");
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

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

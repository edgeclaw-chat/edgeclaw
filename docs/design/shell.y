/* 只含 shell 产生式（提交后的那部分）。期望：0 conflicts。
   验证 chain/pipeline/stage/options/redir 子文法。 */
%token SLASH_CMD QUOTED RAW RAW_REST SIGIL_NEWGROUP SIGIL_LOCALSHELL
%token PIPE SEQ BG OPT
%token REDIR_1 REDIR_1A REDIR_2 REDIR_2A REDIR_0
%%
input      : /* empty */
           | shell
           ;
shell      : chain
           | SIGIL_NEWGROUP chain
           ;
chain      : pipeline
           | chain SEQ pipeline
           | chain BG
           ;
pipeline   : stage
           | pipeline PIPE stage
           ;
stage      : QUOTED options
           ;
options    : /* empty */
           | options OPT
           | options redir
           ;
redir      : REDIR_1 QUOTED
           | REDIR_1A QUOTED
           | REDIR_2 QUOTED
           | REDIR_2A QUOTED
           | REDIR_0 QUOTED
           ;
%%
static int yylex(void) { return 0; }
static void yyerror(const char *s) { (void)s; }
int main(void) { return yyparse() == 0 ? 0 : 1; }

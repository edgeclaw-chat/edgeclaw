/* 带 message 兜底的“天真 CFG”（没有提交点）。期望：大量 shift/reduce 冲突 —— 
   这正好证明 message 不能作为普通产生式存在，必须在词法/分类层做提交（PEG）。 */
%token SLASH_CMD QUOTED RAW RAW_REST SIGIL_NEWGROUP SIGIL_LOCALSHELL
%token PIPE SEQ BG OPT
%token REDIR_1 REDIR_1A REDIR_2 REDIR_2A REDIR_0
%%
input      : /* empty */
           | shell
           | command
           | localshell
           | message
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
redir      : REDIR_1 QUOTED | REDIR_1A QUOTED | REDIR_2 QUOTED | REDIR_2A QUOTED | REDIR_0 QUOTED
           ;
command    : SLASH_CMD toks ;
localshell : SIGIL_LOCALSHELL
           | SIGIL_LOCALSHELL RAW_REST
           ;
message    : tok
           | message tok
           ;
toks       : /* empty */ | toks tok ;
tok        : QUOTED | RAW | PIPE | SEQ | BG | OPT
           | REDIR_1 | REDIR_1A | REDIR_2 | REDIR_2A | REDIR_0
           | SLASH_CMD | SIGIL_NEWGROUP | SIGIL_LOCALSHELL
           ;
%%
static int yylex(void) { return 0; }
static void yyerror(const char *s) { (void)s; }
int main(void) { return yyparse() == 0 ? 0 : 1; }

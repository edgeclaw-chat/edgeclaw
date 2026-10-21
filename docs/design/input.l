/* 词法验证器：把词法规则写成 flex，喂测试行看 token 流。
   简化：T_RAW 按单字符吐；三引号里不处理转义。只为验证“哪些串成了什么 token”。 */
%option noyywrap nodefault 8bit
%x REST
%{
#include <stdio.h>
#include <string.h>
enum { T_SLASH_CMD=256, T_QUOTED_INTERP, T_QUOTED_RAW, T_RAW_REST, T_SIGIL_NEWGROUP,
       T_SIGIL_LOCALSHELL, T_PIPE, T_SEQ, T_BG, T_REDIR_1, T_REDIR_1A, T_REDIR_2,
       T_REDIR_2A, T_REDIR_0, T_OPT, T_RAW };
static const char *charset = "|;&><\"'+-@/2._abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char *name(int t){ switch(t){
  case T_SLASH_CMD: return "SLASH_CMD"; case T_QUOTED_INTERP: return "QUOTED.INTERP";
  case T_QUOTED_RAW: return "QUOTED.RAW"; case T_RAW_REST: return "RAW_REST";
  case T_SIGIL_NEWGROUP: return "SIGIL.+"; case T_SIGIL_LOCALSHELL: return "SIGIL.!";
  case T_PIPE: return "PIPE"; case T_SEQ: return "SEQ"; case T_BG: return "BG";
  case T_REDIR_1: return "REDIR{1}"; case T_REDIR_1A: return "REDIR{1,append}";
  case T_REDIR_2: return "REDIR{2}"; case T_REDIR_2A: return "REDIR{2,append}";
  case T_REDIR_0: return "REDIR{0}"; case T_OPT: return "OPT"; case T_RAW: return "RAW";
  default: return "?"; } }
%}
%%
^"!"                        { BEGIN(REST); return T_SIGIL_LOCALSHELL; }
<REST>.+                    { return T_RAW_REST; }
<REST>\n                    { BEGIN(INITIAL); }
"\"\"\""[^"]*"\"\"\""       { return T_QUOTED_INTERP; }
"'''"[^']*"'''"             { return T_QUOTED_RAW; }
\"([^"\\]|\\.)*\"           { return T_QUOTED_INTERP; }
'([^'\\]|\\.)*'             { return T_QUOTED_RAW; }
"2>>"                       { return T_REDIR_2A; }
"2>"                        { return T_REDIR_2; }
">>"                        { return T_REDIR_1A; }
">"                         { return T_REDIR_1; }
"<"                         { return T_REDIR_0; }
"|"                         { return T_PIPE; }
";"                         { return T_SEQ; }
"&"[ \t]*$                  { return T_BG; }
"--bg"([ \t\n]|$)             { return T_OPT; }
"--new-group"([ \t\n]|$)      { return T_OPT; }
"--with"/[ \t\n]            { return T_OPT; }
"--group"/[ \t\n]           { return T_OPT; }
"-d"/[ \t]                  { return T_OPT; }
"-g"/[ \t]                  { return T_OPT; }
^"/"[A-Za-z_-]+             { return T_SLASH_CMD; }
^"+"                        { return T_SIGIL_NEWGROUP; }
[ \t]                       ;
\n                          ;
[^ \t\n]                    { return T_RAW; }
%%
static void show(const char *line){
  char *buf = calloc(1, strlen(line) + 2); strcpy(buf, line); size_t n = strlen(buf);
  while (n && (buf[n-1]=='\n')) buf[--n] = 0;
  printf("%-24s => ", line);
  strcpy(buf + strlen(buf), "\n");
  YY_BUFFER_STATE b = yy_scan_string(buf); BEGIN(INITIAL);
  int t, guard = 0;
  while ((t = yylex()) != 0 && guard++ < 40) printf("%s ", name(t));
  printf("\n"); yy_delete_buffer(b); free(buf);
}
int main(void){
  show("\"a\" | b");
  show("!git log | head");
  show("2> \"err.log\"");
  show("\"a\" 2>> \"e\"");
  show("&");
  show("\"a\" & x");
  show("--bg");
  show("--bg=1");
  show("-dfoo");
  show("+\"a\"");
  show("+foo");
  show("/task new \"x\" --bg");
  show("\"hi there\"");
  return 0;
}

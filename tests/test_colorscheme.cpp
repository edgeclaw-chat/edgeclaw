// Tests for vim-colorscheme import: the hi parser, the semantic mapping into
// style::Palette, surface repair, and install/list/load in a temporary home.

#include "edc_colorscheme.h"
#include "edc_colorscheme_palette.hpp"
#include "edc_style.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#else
#include <direct.h>
#endif

static int g_fail = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                    \
        {                                                                                                               \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            g_fail++;                                                                                                   \
        }                                                                                                               \
    } while (0)

static void makedir(const std::string &p)
{
#if !defined(_WIN32)
    ::mkdir(p.c_str(), 0700);
#else
    ::_mkdir(p.c_str());
#endif
}

static void writefile(const std::string &path, const std::string &body)
{
    FILE *f = ::fopen(path.c_str(), "wb");
    if (f)
    {
        ::fwrite(body.data(), 1, body.size(), f);
        ::fclose(f);
    }
}

static bool rgb_is(const ftxui::Color &c, unsigned r, unsigned g, unsigned b)
{
    // ftxui quantises to the terminal depth; tests run at default (truecolor-like)
    // depth, and we only compare colours we constructed ourselves here.
    return c == ftxui::Color::RGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static void test_parse_and_map()
{
    std::printf("  parse: hi commands, links, NONE, hex forms\n");
    const char *vim = "set background=dark\n"
                      "hi clear\n"
                      "if exists('syntax_on') | syntax reset | endif\n"
                      "let g:colors_name = 'demo'\n"
                      "hi Normal guifg=#d0d0d0 guibg=#1c1c1c\n"
                      "hi Comment guifg=#6a6a6a\n"
                      "hi link NonText Comment\n"
                      "hi Keyword guifg=#ff00aa\n"
                      "hi ErrorMsg guifg=#ff0000 guibg=NONE\n"
                      "hi Pmenu guibg=#333333 guifg=#ffffff\n"
                      "hi markdownH1 guifg=#00ff00\n";

    edc_colorscheme cs;
    CHECK(edc_colorscheme_parse(vim, std::strlen(vim), &cs), "vim parsed");

    CHECK(cs.set[EDC_CS_PANE_BG] && cs.rgb[EDC_CS_PANE_BG] == 0x1c1c1c, "Normal bg -> pane");
    CHECK(cs.set[EDC_CS_TEXT] && cs.rgb[EDC_CS_TEXT] == 0xd0d0d0, "Normal fg -> text");
    CHECK(cs.set[EDC_CS_TEXT_DIM] && cs.rgb[EDC_CS_TEXT_DIM] == 0x6a6a6a, "Comment -> text_dim");
    CHECK(cs.set[EDC_CS_ACCENT] && cs.rgb[EDC_CS_ACCENT] == 0xff00aa, "Keyword -> accent");
    CHECK(cs.set[EDC_CS_ERROR] && cs.rgb[EDC_CS_ERROR] == 0xff0000, "ErrorMsg -> error");
    CHECK(cs.set[EDC_CS_MD_H1] && cs.rgb[EDC_CS_MD_H1] == 0x00ff00, "markdownH1 -> md_h1");
    CHECK(cs.set[EDC_CS_DIALOG_BG] && cs.rgb[EDC_CS_DIALOG_BG] == 0x333333, "Pmenu bg -> dialog");
    // NONE must not be used as a colour; the role falls back to a built-in.
    CHECK(!cs.set[EDC_CS_MD_H1_BG], "NONE is not a parsed colour");

    std::printf("  map: sparse roles fall back, surfaces stay distinct\n");
    edc::style::Palette p = edc::theme::PaletteFromScheme(cs, edc::style::dark_palette(), false);
    CHECK(rgb_is(p.pane_bg, 0x1c, 0x1c, 0x1c), "mapped pane bg");
    CHECK(rgb_is(p.text, 0xd0, 0xd0, 0xd0), "mapped text");
    // window_bg and pane_bg both came from Normal's bg; the repair must widen them.
    CHECK(!(p.window_bg == p.pane_bg), "window_bg repaired off pane_bg");
    // md_code was not in the file: it keeps the built-in dark palette's value.
    CHECK(p.md_code == edc::style::dark_palette().md_code, "unset role keeps fallback");
}

static void test_thought_closed_is_dimmed_open()
{
    std::printf("  thought: collapsed is a dimmed version of the expanded theme colour\n");
    // Todo -> thought_open (purple), Comment -> would-be thought_closed (grey).
    // A pure import would leave collapsed grey; the provider must re-derive it
    // from the open colour so both states read as the same theme hue.
    const char *vim = "hi Normal guifg=#ffffff guibg=#101018\n"
                      "hi Todo guifg=#bd5eff\n"     // thought_open -> purple
                      "hi Comment guifg=#6a6a6a\n"; // would-be thought_closed -> grey
    edc_colorscheme cs;
    CHECK(edc_colorscheme_parse(vim, std::strlen(vim), &cs), "thought theme parsed");
    const edc::style::Palette p = edc::theme::PaletteFromScheme(cs, edc::style::dark_palette(), false);

    // thought_open is the theme purple verbatim.
    CHECK(rgb_is(p.thought_open, 0xbd, 0x5e, 0xff), "thought_open is the theme colour");
    // thought_closed is a slightly dimmed version of that SAME purple: mix(open, pane, 0.25).
    // open=#bd5eff, pane=#101018 -> approx #944ac9 (still bright purple, not grey/washed out).
    CHECK(!(p.thought_closed == p.thought_open), "collapsed differs from expanded");
    CHECK(p.thought_closed != edc::style::dark_palette().thought_closed, "imported closed != built-in yellow");
    const auto mix = [](int a, int b) { return static_cast<unsigned char>(a + (b - a) * 0.25f); };
    CHECK(rgb_is(p.thought_closed, mix(0xbd, 0x10), mix(0x5e, 0x10), mix(0xff, 0x18)),
          "collapsed = slightly dimmed purple toward the pane, not the grey Comment");
}

static void test_reject_and_named()
{
    std::printf("  parse: unsupported / named colours / bad names\n");
    const char *lua_theme = "local M = {}\nvim.api.nvim_set_hl(0, 'Normal', {fg='#ffffff'})\nreturn M\n";
    edc_colorscheme cs;
    CHECK(!edc_colorscheme_parse(lua_theme, std::strlen(lua_theme), &cs), "Lua-only theme rejected");

    const char *named = "hi Normal guifg=White guibg=Black\nhi Comment guifg=red\n";
    CHECK(edc_colorscheme_parse(named, std::strlen(named), &cs), "named colours parsed");
    CHECK(cs.rgb[EDC_CS_PANE_BG] == 0x000000, "black bg");
    CHECK(cs.rgb[EDC_CS_TEXT] == 0xffffff, "white fg");
    CHECK(cs.rgb[EDC_CS_TEXT_DIM] == 0xff0000, "red comment");

    // `hi!` (the bang redefine) is ubiquitous in real themes. `warn` maps from
    // WarningMsg then Todo; Todo has no colour of its own, only a bang-link to
    // Special. If `hi! link` is parsed, warn resolves to Special's colour; if the
    // bang line is skipped, warn stays unset. This isolates the bang handling.
    const char *bang = "hi Normal guifg=#ffffff guibg=#000000\n"
                       "hi Special guifg=#abcdef\n"
                       "hi! link Todo Special\n";
    CHECK(edc_colorscheme_parse(bang, std::strlen(bang), &cs), "hi! parsed");
    CHECK(cs.set[EDC_CS_WARN] && cs.rgb[EDC_CS_WARN] == 0xabcdef, "hi! link resolves (bang handled)");

    CHECK(!edc_colorscheme_path("/home/u/.edgeclaw", "../escape", nullptr, 0), "path traversal refused");
    char name[8];
    CHECK(!edc_colorscheme_install("/x", "/no/such/file.vim", name, sizeof(name), nullptr, 0), "missing source refused");
}

static void test_install_list_load()
{
    std::printf("  dir: install -> list -> load -> path guard\n");
    const std::string home = "/tmp/edc_cs_test_home";
    makedir(home);
    // Clean out a prior run's colors dir.
    std::string colors = home + "/colors";
    makedir(colors);
    const std::string src = "/tmp/edc_cs_source.vim";
    writefile(src, "hi Normal guifg=#abcdef guibg=#101010\nhi Keyword guifg=#123456\n");

    char installed[128] = {0};
    char err[256] = {0};
    CHECK(edc_colorscheme_install(home.c_str(), src.c_str(), installed, sizeof(installed), err, sizeof(err)),
          "install succeeded");
    CHECK(std::string(installed) == "edc_cs_source", "name from basename");

    size_t n = edc_colorscheme_list(home.c_str(), nullptr, nullptr);
    CHECK(n == 1, "one theme listed");

    edc_colorscheme loaded;
    CHECK(edc_colorscheme_load(home.c_str(), installed, &loaded), "load installed theme");
    CHECK(loaded.set[EDC_CS_TEXT] && loaded.rgb[EDC_CS_TEXT] == 0xabcdef, "loaded fg correct");

    CHECK(!edc_colorscheme_load(home.c_str(), "does-not-exist", &loaded), "missing theme refused");
    CHECK(!edc_colorscheme_load(home.c_str(), "off", &loaded), "off is not a file");

    ::remove((colors + "/edc_cs_source.vim").c_str());
    ::remove(src.c_str());
}

int main()
{
    std::printf("colorscheme tests\n");
    test_parse_and_map();
    test_thought_closed_is_dimmed_open();
    test_reject_and_named();
    test_install_list_load();
    if (g_fail)
    {
        std::printf("%d checks failed\n", g_fail);
        return 1;
    }
    std::printf("all colorscheme tests passed\n");
    return 0;
}

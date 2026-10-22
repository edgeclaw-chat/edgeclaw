// Unit tests for the light/dark palette selection (edc_theme.* + the palette
// factory in edc_style.hpp).
//
// The TUI paints every surface it draws, so it cannot lean on the terminal's
// default foreground to match those surfaces: on a light-themed terminal that
// foreground is black and the body text disappears into the dark panes. Which
// palette to use is therefore decided from the terminal's own background,
// queried over OSC 11 at startup. These tests pin the three halves of that:
// parsing the reply, deciding light vs dark from it, and the legibility of the
// light palette itself.

#include "edc_style.hpp"
#include "edc_theme.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/terminal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h> // openpty
#else
#include <pty.h> // openpty
#endif
#endif

static int g_failures = 0;

// Cross-platform environment write for the override tests.
static void SetEnv(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value && *value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        std::printf("  %s\n", name);                                                                                   \
    } while (0)

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            std::printf("    ");                                                                                       \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

using edc::theme::Mode;

// --- WCAG contrast, so the light palette is checked, not just eyeballed ---

struct Rgb
{
    unsigned char r, g, b;
};

Rgb Hex(const char *s)
{
    auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    return Rgb{static_cast<unsigned char>((nibble(s[0]) << 4) | nibble(s[1])),
               static_cast<unsigned char>((nibble(s[2]) << 4) | nibble(s[3])),
               static_cast<unsigned char>((nibble(s[4]) << 4) | nibble(s[5]))};
}

double Channel(unsigned char c)
{
    const double v = c / 255.0;
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double Luminance(Rgb c)
{
    return 0.2126 * Channel(c.r) + 0.7152 * Channel(c.g) + 0.0722 * Channel(c.b);
}

double Contrast(Rgb a, Rgb b)
{
    const double la = Luminance(a);
    const double lb = Luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

void test_parse_mode(void)
{
    TEST("$EDGECLAW_THEME parsing accepts only real theme names");
    Mode m = Mode::kDark;
    CHECK(edc::theme::ParseMode("dark", m) && m == Mode::kDark, "plain dark rejected");
    CHECK(edc::theme::ParseMode("light", m) && m == Mode::kLight, "plain light rejected");
    CHECK(edc::theme::ParseMode("LIGHT", m) && m == Mode::kLight, "case not ignored");
    CHECK(edc::theme::ParseMode("  light\t", m) && m == Mode::kLight, "whitespace not trimmed");

    // "auto" (and anything unrecognized) means "ask the terminal", which is
    // signalled by returning false rather than by a third mode.
    CHECK(!edc::theme::ParseMode("auto", m), "auto should fall through to the probe");
    CHECK(!edc::theme::ParseMode("", m), "empty name accepted");
    CHECK(!edc::theme::ParseMode("lightt", m), "prefix accepted");
    CHECK(!edc::theme::ParseMode("dark mode", m), "two words accepted");
}

void test_parse_osc11(void)
{
    TEST("OSC 11 replies parse in every form terminals actually send");
    unsigned char r = 0, g = 0, b = 0;

    // What Terminal.app answers (BEL-terminated, 16-bit components). Captured
    // from a live probe: ESC ] 11 ; rgb:1e1e/1e1e/1e1e BEL.
    CHECK(edc::theme::ParseOsc11("\x1b]11;rgb:1e1e/1e1e/1e1e\x07", r, g, b), "Terminal.app reply rejected");
    CHECK(r == 0x1e && g == 0x1e && b == 0x1e, "Terminal.app reply decoded as %02x/%02x/%02x", r, g, b);

    // 8-bit components, ST-terminated.
    CHECK(edc::theme::ParseOsc11("\x1b]11;rgb:ff/ff/ff\x1b\\", r, g, b), "8-bit reply rejected");
    CHECK(r == 255 && g == 255 && b == 255, "8-bit reply decoded as %02x/%02x/%02x", r, g, b);

    // CSS-ish forms.
    CHECK(edc::theme::ParseOsc11("\x1b]11;#122738\x1b\\", r, g, b), "hex reply rejected");
    CHECK(r == 0x12 && g == 0x27 && b == 0x38, "hex reply decoded as %02x/%02x/%02x", r, g, b);
    CHECK(edc::theme::ParseOsc11("\x1b]11;#fff\x1b\\", r, g, b), "short hex reply rejected");
    CHECK(r == 255 && g == 255 && b == 255, "short hex reply decoded as %02x/%02x/%02x", r, g, b);

    // Noise in front (ftxui asks for the cursor position too) must not confuse it.
    CHECK(edc::theme::ParseOsc11("\x1b[12;40R\x1b]11;rgb:0000/0000/0000\x07", r, g, b), "reply after noise rejected");
    CHECK(r == 0 && g == 0 && b == 0, "reply after noise decoded as %02x/%02x/%02x", r, g, b);

    // Still arriving: the reader keeps accumulating until a terminator shows up.
    CHECK(!edc::theme::ParseOsc11("\x1b]11;rgb:1e1e/1e1e", r, g, b), "incomplete reply accepted");
    CHECK(!edc::theme::ParseOsc11("\x1b[12;40R", r, g, b), "unrelated report accepted");
    CHECK(!edc::theme::ParseOsc11("", r, g, b), "empty buffer accepted");
}

void test_light_background(void)
{
    TEST("the background decides light vs dark");
    CHECK(!edc::theme::IsLightBackground(0x1e, 0x1e, 0x1e), "#1e1e1e should be dark");
    CHECK(!edc::theme::IsLightBackground(0x27, 0x28, 0x22), "Monokai pane should be dark");
    CHECK(!edc::theme::IsLightBackground(0x12, 0x27, 0x38), "Ghostty background should be dark");
    CHECK(!edc::theme::IsLightBackground(0, 0, 0), "black should be dark");
    CHECK(edc::theme::IsLightBackground(255, 255, 255), "white should be light");
    CHECK(edc::theme::IsLightBackground(0xf8, 0xf8, 0xf2), "off-white should be light");
    CHECK(edc::theme::IsLightBackground(0xe8, 0xe8, 0xe1), "light window_bg should be light");
}

void test_palette_selection(void)
{
    TEST("palette() follows the resolved mode");
    edc::theme::SetMode(Mode::kDark);
    CHECK(!edc::theme::IsLight(), "kDark should not report light");
    CHECK(edc::style::palette().pane_bg == edc::style::dark_palette().pane_bg, "kDark did not select the dark palette");

    edc::theme::SetMode(Mode::kLight);
    CHECK(edc::theme::IsLight(), "kLight should report light");
    CHECK(edc::style::palette().pane_bg == edc::style::light_palette().pane_bg,
          "kLight did not select the light palette");

    // The two must actually differ in both directions (a pale background and a
    // dark foreground), or the switch would be cosmetic.
    CHECK(edc::style::dark_palette().pane_bg != edc::style::light_palette().pane_bg, "palettes share a pane colour");
    CHECK(edc::style::dark_palette().text != edc::style::light_palette().text, "palettes share a text colour");
}

#if !defined(_WIN32)

// Drives the real probe against a pseudo-terminal that we answer ourselves. The
// parser is covered above; this covers the tty plumbing (raw mode, writing the
// query, the poll loop, restoring the terminal), which is the part that cannot
// be checked by reading it.
void test_query_terminal_background(void)
{
    TEST("the probe drives a terminal end to end");

    int master = -1;
    int slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0)
    {
        std::printf("    (no pty available, skipping)\n");
        return;
    }

    const pid_t child = fork();
    if (child < 0)
    {
        std::printf("    (fork failed, skipping)\n");
        close(master);
        close(slave);
        return;
    }
    if (child == 0)
    {
        // Stand in for the terminal: wait for the OSC 11 query, then answer it.
        close(slave);
        std::string seen;
        char c = 0;
        while (read(master, &c, 1) == 1)
        {
            seen += c;
            if (seen.find("]11;?") != std::string::npos)
                break;
        }
        const char reply[] = "\x1b]11;rgb:f8f8/f8f8/f2f2\x07";
        const ssize_t written = write(master, reply, sizeof(reply) - 1);
        // Linger before closing the master: closing it hangs up the pty, and a
        // hangup discards whatever the reader has not picked up yet. A real
        // terminal stays open, so the harness must too.
        usleep(250 * 1000);
        close(master);
        _exit(written == static_cast<ssize_t>(sizeof(reply) - 1) ? 0 : 1);
    }

    close(master);
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
    const bool ok = edc::theme::QueryTerminalBackground(slave, 500, r, g, b);
    close(slave);
    int status = 0;
    waitpid(child, &status, 0);

    CHECK(ok, "probe did not read the reply");
    CHECK(r == 0xf8 && g == 0xf8 && b == 0xf2, "probe decoded %02x/%02x/%02x", r, g, b);
    CHECK(edc::theme::IsLightBackground(r, g, b), "an off-white reply should read as light");

    // A terminal that stays silent must not hang the probe: the timeout is the
    // only thing standing between a dumb tty and a stuck startup.
    int m2 = -1;
    int s2 = -1;
    if (openpty(&m2, &s2, nullptr, nullptr, nullptr) == 0)
    {
        const auto began = std::chrono::steady_clock::now();
        const bool silent = edc::theme::QueryTerminalBackground(s2, 120, r, g, b);
        const auto spent =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
        close(m2);
        close(s2);
        CHECK(!silent, "a silent terminal should report no answer");
        CHECK(spent.count() < 2000, "the silent path took %lld ms", static_cast<long long>(spent.count()));
    }

    // Not a tty at all: report "unknown" rather than writing into a pipe.
    int fds[2];
    if (pipe(fds) == 0)
    {
        CHECK(!edc::theme::QueryTerminalBackground(fds[0], 50, r, g, b), "a pipe should not be probed");
        close(fds[0]);
        close(fds[1]);
    }
}

#endif // !_WIN32

#if !defined(_WIN32)

// $EDGECLAW_THEME outranks the probe, and an unusable value must not stop Init()
// from working -- it falls through to the probe, which under a test harness has
// no tty to ask and therefore keeps the default.
void test_init_env_override(void)
{
    TEST("$EDGECLAW_THEME outranks the probe");

    edc::theme::SetMode(Mode::kDark);
    setenv("EDGECLAW_THEME", "light", 1);
    edc::theme::Init();
    CHECK(edc::theme::IsLight(), "light override ignored");

    setenv("EDGECLAW_THEME", "LIGHT", 1);
    edc::theme::Init();
    CHECK(edc::theme::IsLight(), "case-insensitive override ignored");

    edc::theme::SetMode(Mode::kLight);
    setenv("EDGECLAW_THEME", "dark", 1);
    edc::theme::Init();
    CHECK(!edc::theme::IsLight(), "dark override ignored");

    edc::theme::SetMode(Mode::kDark);
    setenv("EDGECLAW_THEME", "auto", 1);
    edc::theme::Init();
    CHECK(!edc::theme::IsLight(), "an unusable value left no default in place");

    unsetenv("EDGECLAW_THEME");
    edc::theme::Init();
    CHECK(!edc::theme::IsLight(), "an unset variable left no default in place");
}

#endif // !_WIN32

void test_light_palette_contrast(void)
{
    TEST("the light palette is Monokai Light, and nothing in it is invisible");

    const auto &light = edc::style::light_palette();
    const Rgb pane = Hex("ffffff");

    // Every value below is verbatim from rjfranco's Monokai Light
    // (https://github.com/rjfranco/monokai-light) -- pinning them keeps the
    // palette honest about its source. The ratios are what that theme really
    // gives on white: its syntax colours sit at 2.0-3.8:1, below WCAG AA for
    // small text by design, so the floor asserted here is "not invisible"
    // rather than 4.5. Only the body text runs high-contrast.
    struct Role
    {
        const char *name;
        ftxui::Color color;
        const char *hex;
        const char *on_hex; // surface it is drawn on; null means pane_bg
        double min_ratio;
    };
    const Role roles[] = {
        {"text", light.text, "000000", nullptr, 10.0},          // 21.0  foreground
        {"text_dim", light.text_dim, "9f9f8f", nullptr, 2.0},   // 2.68  comment
        {"accent", light.accent, "28c6e4", nullptr, 2.0},       // 2.04  support.type
        {"ok", light.ok, "6aaf19", nullptr, 2.0},               // 2.71  function / class
        {"warn", light.warn, "fd971f", nullptr, 2.0},           // 2.18  function argument
        {"error", light.error, "f92672", nullptr, 2.0},         // 3.79  keyword
        {"prompt", light.prompt, "28c6e4", nullptr, 2.0},
        {"user_rule", light.user_rule, "28c6e4", nullptr, 2.0},
        {"thought_open", light.thought_open, "8a8345", nullptr, 2.0},     // 3.88  the dark palette's yellow, kept
        {"thought_closed", light.thought_closed, "a89a5e", nullptr, 2.0}, // 2.82  quieter yellow
        {"md_h1", light.md_h1, "f8f8f0", "f92672", 2.0},                  // 3.55  on its banner
        {"md_h1_bg", light.md_h1_bg, "f92672", nullptr, 2.0},
        {"md_h2", light.md_h2, "f25a00", nullptr, 2.0}, // 3.37  string
        {"md_h3", light.md_h3, "fd971f", nullptr, 2.0},
        {"md_h4", light.md_h4, "28c6e4", nullptr, 2.0},
        {"md_code", light.md_code, "000000", nullptr, 10.0},
        {"md_code_inline", light.md_code_inline, "ae81ff", "ededed", 2.0}, // 2.43  on its chip
        {"md_quote", light.md_quote, "9f9f8f", nullptr, 2.0},
        {"md_list", light.md_list, "f92672", nullptr, 2.0},
        {"md_link", light.md_link, "28c6e4", nullptr, 2.0},
    };
    for (const Role &role : roles)
    {
        const Rgb rgb = Hex(role.hex);
        CHECK(role.color == ftxui::Color::RGB(rgb.r, rgb.g, rgb.b), "%s is not #%s", role.name, role.hex);
        const Rgb surface = role.on_hex ? Hex(role.on_hex) : pane;
        const double ratio = Contrast(rgb, surface);
        CHECK(ratio >= role.min_ratio, "%s contrast %.2f < %.2f", role.name, ratio, role.min_ratio);
    }

    // The surfaces come from the theme too, but no longer verbatim: it keeps Monokai
    // Light's RELATIONSHIP (the pane is the lighter one, the window sits behind it)
    // and gives up two of its values, because both were indistinguishable from what
    // they sit next to. inactiveSelection #EDEDED was 1.17:1 from the white pane, and
    // invisibles #E0E0E0 would now be the backdrop colour itself -- a rule in it would
    // vanish into the gutter the borderless split draws with. See
    // test_surface_separation for the floor both palettes are held to.
    CHECK(light.window_bg == ftxui::Color::RGB(223, 223, 223), "window_bg drifted from its pinned value");
    CHECK(light.line_dim == ftxui::Color::RGB(204, 204, 204), "line_dim drifted from its pinned value");
    const Rgb window = Hex("dfdfdf");
    CHECK(Luminance(pane) > Luminance(window), "pane should be lighter than the window backdrop");
    CHECK(Contrast(pane, window) >= edc::style::kMinSurfaceSeparation, "pane and window collapsed into one tone");
}

// The shared "working" animation curve (style::breath_phase), used by both the
// processing task dot and the in_progress todo row.
void test_breath_curve(void)
{
    TEST("breath curve");
    const long long period = edc::style::kBreathPeriodMs;
    CHECK(edc::style::breath_phase(0) == 0.0f, "the cycle starts at the dim end");
    CHECK(std::fabs(edc::style::breath_phase(period / 2) - 1.0f) < 1e-6f, "it peaks mid-cycle");
    const float quarter = edc::style::breath_phase(period / 4);
    CHECK(quarter > 0.0f && quarter < 1.0f, "the rise passes through the middle");
    CHECK(edc::style::breath_phase(period) == edc::style::breath_phase(0), "it repeats each period");
    CHECK(edc::style::breath_phase(-100) == edc::style::breath_phase(period - 100),
          "a negative clock wraps into the same phase");
}

// ftxui's Color::Interpolate (a per-channel gamma-2.2 lerp truncated to 8 bit)
// re-stated here, so the endpoint colours the breath paints can be measured
// rather than eyeballed.
Rgb Blend(double t, Rgb a, Rgb b)
{
    auto one = [t](unsigned char x, unsigned char y) {
        const double c = std::pow(static_cast<double>(x), 2.2) * (1.0 - t) + std::pow(static_cast<double>(y), 2.2) * t;
        return static_cast<unsigned char>(std::pow(c, 1.0 / 2.2));
    };
    return Rgb{one(a.r, b.r), one(a.g, b.g), one(a.b, b.b)};
}

ftxui::Color RgbColor(Rgb c)
{
    return ftxui::Color::RGB(c.r, c.g, c.b);
}

/* The borderless layout has no frames, so a boundary exists only if the two surfaces
 * it separates differ enough to see. That is the whole design: pane on backdrop, and a
 * hairline on both. The palette originally failed it -- #1E1F1C against #272822 is
 * 1.11:1, i.e. nothing, and exactly nothing on a terminal themed after Monokai, whose
 * own background IS #272822 (the pane colour). Every component then sat on one flat
 * field and no block edge could be found. This pins the floor for both palettes and
 * for the rule colour, so the failure cannot come back quietly.
 *
 * Ratios are WCAG contrast between two SURFACES, not text: the bar is the point where
 * an edge reads on an ordinary panel (a macOS-style sidebar sits at ~1.2:1), not AA. */
void test_surface_separation(void)
{
    TEST("every surface a boundary is drawn with is a visible step from its neighbour");

    struct Case
    {
        const char *name;
        const edc::style::Palette &pal;
        Rgb window;   // the backdrop / split gutter
        Rgb pane;     // the panel surface
        Rgb line;     // rules and hairlines
        Rgb raised;   // dialog_bg: modals and popups floating over a pane
    };
    const Case cases[] = {
        {"dark", edc::style::dark_palette(), Hex("0e0f0d"), Hex("272822"), Hex("49483e"), Hex("3e3d32")},
        {"light", edc::style::light_palette(), Hex("dfdfdf"), Hex("ffffff"), Hex("cccccc"), Hex("e2e2e2")},
    };

    for (const Case &c : cases)
    {
        // The hexes restated above must stay the ones the widgets draw with.
        CHECK(RgbColor(c.window) == c.pal.window_bg, "%s: the test's backdrop drifted from window_bg", c.name);
        CHECK(RgbColor(c.pane) == c.pal.pane_bg, "%s: the test's pane drifted from pane_bg", c.name);
        CHECK(RgbColor(c.line) == c.pal.line_dim, "%s: the test's rule colour drifted from line_dim", c.name);
        CHECK(RgbColor(c.raised) == c.pal.dialog_bg, "%s: the test's raised surface drifted from dialog_bg", c.name);

        // In both themes the pane is the lighter surface: the backdrop recedes.
        CHECK(Luminance(c.pane) > Luminance(c.window), "%s: the backdrop is not behind the pane", c.name);
        CHECK(Contrast(c.pane, c.window) >= edc::style::kMinSurfaceSeparation,
              "%s: pane and backdrop are %.2f:1 apart, below the %.2f floor -- the split gutter "
              "disappears and the panes merge into one field",
              c.name, Contrast(c.pane, c.window), edc::style::kMinSurfaceSeparation);

        // The hairline is drawn in the gutter (splitting the panes) and on the pane
        // (section rules), so it has to be readable on both -- dimmer than the text,
        // but an edge, not a rumour.
        CHECK(Contrast(c.line, c.window) >= 1.2, "%s: the rule is lost in the gutter (%.2f:1)", c.name,
              Contrast(c.line, c.window));
        CHECK(Contrast(c.line, c.pane) >= 1.4, "%s: the rule is lost on the pane (%.2f:1)", c.name,
              Contrast(c.line, c.pane));

        // A popup floats over a pane, so its surface has to separate from the pane --
        // and the pane is only one of the two it can land on (the chat is pane_bg from
        // edge to edge now, which is exactly why the completion popup cannot be).
        CHECK(Contrast(c.raised, c.pane) >= edc::style::kMinSurfaceSeparation,
              "%s: a popup on the pane has no surface of its own (%.2f:1)", c.name, Contrast(c.raised, c.pane));
    }
}

/* What the seam is actually made of, drawn rather than reasoned about: two cells --
 * one of backdrop, one hairline -- because the gutter is also ResizableSplit's drag
 * handle and a mark on a grabbable column is honest. The cells carry no background of
 * their own: they let the window backdrop (painted over the whole frame in
 * TUIWindow::OnRender) show through, which is what makes the gutter read as the gap
 * BETWEEN two panels rather than as a third panel. */
void test_split_gutter(void)
{
    TEST("the split gutter is one backdrop cell plus one hairline cell");

    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(2), ::ftxui::Dimension::Fixed(1));
    ::ftxui::Render(screen, edc::style::split_gutter());

    CHECK(screen.PixelAt(0, 0).character == " ", "the gutter's first cell is not a space");
    CHECK(screen.PixelAt(1, 0).character == "│", "the gutter's second cell is not a hairline");
    CHECK(screen.PixelAt(1, 0).foreground_color == edc::style::palette().line_dim,
          "the hairline is not drawn in line_dim");
    for (int x = 0; x < 2; x++)
    {
        CHECK(screen.PixelAt(x, 0).background_color == ::ftxui::Color::Default,
              "gutter cell %d paints a background: the backdrop has to show through", x);
    }
}


// The breath swings between the pane surface and the theme's own accent: the
// trough is the accent faded into the surface, the peak is the accent untouched
// -- never brighter. That keeps the top of every cycle the same blue the section
// titles, the input prompt and the working bar wear, and it keeps the glyph from
// washing out to white, which is what pushing past the accent did (the accent
// already sits at HSV value 0.94, so extra brightness is paid for in
// saturation). Pinned per palette, since the pane is dark on one and white on
// the other.
void test_breath_endpoints(void)
{
    TEST("the breath swings between the pane surface and the theme accent");

    struct Case
    {
        const char *name;
        const edc::style::Palette &pal;
        Rgb pane;
        Rgb accent;
    };
    const Case cases[] = {
        {"dark", edc::style::dark_palette(), Hex("272822"), Hex("66d9ef")},
        {"light", edc::style::light_palette(), Hex("ffffff"), Hex("28c6e4")},
    };

    for (const Case &c : cases)
    {
        // The palette constants restated above must stay the ones the widget
        // draws with, or the measurement would drift away from the screen.
        CHECK(RgbColor(c.pane) == c.pal.pane_bg, "the test's pane colour drifted from the palette");
        CHECK(RgbColor(c.accent) == c.pal.accent, "the test's accent drifted from the palette");

        const Rgb dim = Blend(edc::style::kBreathDimFade, c.accent, c.pane);
        CHECK(edc::style::breath_dim(c.pal) == RgbColor(dim), "the dim end is not the accent faded into the surface");

        const double plain = Contrast(c.accent, c.pane);
        const double lowest = Contrast(dim, c.pane);
        // The trough really is dimmer, and by a fixed fraction of the way to the
        // pane: the equality above pins kBreathDimFade, so the swing's size is
        // pinned with it (measuring it as a contrast *ratio* instead would just
        // re-encode the palette -- the light theme's trough is nearly white, so
        // its ratio is much smaller than the dark one's for the same fade).
        CHECK(lowest < plain, "the trough is no closer to the pane than the plain accent");
    }

    // The end of the rise is the plain accent -- exactly, not nearly: ftxui's
    // per-channel gamma round-trip lands a unit short at t = 1, so the two
    // extremes are returned outright. Read back through palette(), which is what
    // the widgets call, so the whole path from the theme to the colour is
    // covered rather than just the helpers.
    edc::theme::SetMode(Mode::kDark);
    const auto &dark = edc::style::palette();
    CHECK(edc::style::breath_accent(0) == edc::style::breath_dim(dark), "the cycle starts at the dim end");
    CHECK(edc::style::breath_accent(edc::style::kBreathPeriodMs / 2) == dark.accent,
          "the peak is exactly the theme's accent, not a brighter shade of it");
    CHECK(edc::style::breath_accent(edc::style::kBreathPeriodMs / 4) != dark.accent,
          "the rise passes through an intermediate shade on the way up");
}

// The one task-state vocabulary the widgets share: the tree paints its markers
// with this table and the todo list reads its current step's failure colour from
// it, so a test here is what keeps "the task failed" meaning one colour in both
// panes.
void test_task_state_table(void)
{
    TEST("task states: one predicate set, one colour table");

    // Processing: exactly the two states the scheduler calls running.
    CHECK(edc::style::TaskStateProcessing("LLM_PROCESSING"), "LLM_PROCESSING is processing");
    CHECK(edc::style::TaskStateProcessing("TOOL_CALLING"), "TOOL_CALLING is processing");
    CHECK(!edc::style::TaskStateProcessing("IDLE"), "IDLE is not");
    CHECK(!edc::style::TaskStateProcessing("ERROR"), "and neither is a failed task");

    // Failure: the one state whose rows and markers turn red.
    CHECK(edc::style::TaskStateFailed("ERROR"), "ERROR is the failure state");
    CHECK(!edc::style::TaskStateFailed("CANCELLED"), "a cancelled task did not fail");
    CHECK(!edc::style::TaskStateFailed(""), "an unknown owner (no task) did not fail either");

    for (const auto mode : {Mode::kDark, Mode::kLight})
    {
        edc::theme::SetMode(mode);
        const auto &pal = edc::style::palette();

        // The mapping the task tree draws with. COMPLETED ok, ERROR error,
        // processing accent, CANCELLED neutral dim, and the
        // three waiting/idle states share warn (a known trade-off). INTERRUPTED takes
        // the error colour too; the marker's shape is what tells it from ERROR.
        CHECK(edc::style::TaskStateColor(pal, "COMPLETED") == pal.ok, "COMPLETED -> ok");
        CHECK(edc::style::TaskStateColor(pal, "ERROR") == pal.error, "ERROR -> error");
        CHECK(edc::style::TaskStateColor(pal, "LLM_PROCESSING") == pal.accent, "processing -> accent");
        CHECK(edc::style::TaskStateColor(pal, "TOOL_CALLING") == pal.accent, "tool calls -> accent too");
        // INTERRUPTED is orange, not the failure red: the loop stopped underneath the task
        // and no decision was made about it, which is a different fact from "it failed".
        CHECK(edc::style::TaskStateColor(pal, "INTERRUPTED") == pal.interrupted, "INTERRUPTED -> orange");
        CHECK(pal.interrupted != pal.error, "interrupted and ERROR are distinct colours");
        CHECK(edc::style::TaskStateColor(pal, "CANCELLED") == pal.text_dim, "CANCELLED -> neutral dim");
        CHECK(edc::style::TaskStateColor(pal, "IDLE") == pal.warn, "IDLE -> warn");
        CHECK(edc::style::TaskStateColor(pal, "WAITING_DEPS") == pal.warn, "WAITING_DEPS -> warn");
        CHECK(edc::style::TaskStateColor(pal, "WAITING_INPUT") == pal.warn, "WAITING_INPUT -> warn");

        // The failure colour is not the accent, so the todo list's current step
        // cannot be mistaken for a running one when its task has failed.
        CHECK(pal.error != pal.accent, "failure and the running accent are distinct");

        // The vocabulary a reply's code span is matched against (markdown.cpp): every state
        // the scheduler can print is in it, and ordinary code stays code.
        for (const char *name :
             {"COMPLETED", "ERROR", "INTERRUPTED", "CANCELLED", "IDLE", "WAITING_DEPS", "WAITING_INPUT",
              "LLM_PROCESSING", "TOOL_CALLING"})
            CHECK(edc::style::IsTaskStateName(name), "the state vocabulary covers %s", name);
        CHECK(!edc::style::IsTaskStateName("COMPLETED_"), "a near-miss is not a state");
        CHECK(!edc::style::IsTaskStateName("bazel build"), "ordinary inline code stays code");
        CHECK(!edc::style::IsTaskStateName(""), "empty is not a state");
    }
    edc::theme::SetMode(Mode::kDark);
}

} // namespace

// The palette provider seam: style::palette() is served through an IPalette, so a
// test (or a themed renderer) can replace it wholesale. This is what IPalette is
// for -- without a consumer it would be dead code.
struct FakePalette : edc::style::IPalette
{
    edc::style::Palette pal;
    explicit FakePalette(edc::style::Palette p) : pal(std::move(p))
    {
    }
    const edc::style::Palette &colors() const override
    {
        return pal;
    }
};

void test_palette_provider(void)
{
    TEST("IPalette provider seam");
    const edc::style::Palette real = edc::style::palette();
    edc::style::Palette fake = real;
    fake.accent = ftxui::Color::RGB(1, 2, 3);
    fake.pane_bg = ftxui::Color::RGB(4, 5, 6);

    auto provider = std::make_shared<FakePalette>(fake);
    edc::style::SetPaletteProvider(provider);
    CHECK(edc::style::palette().accent == fake.accent, "the installed provider supplies the palette");
    CHECK(edc::style::palette().pane_bg == fake.pane_bg, "every role comes from the provider");

    edc::style::SetPaletteProvider(nullptr);
    CHECK(edc::style::palette().accent == real.accent, "nullptr restores the built-in theme palette");
}

/* The depth has to be settled before the first palette is built: Color::RGB()
 * quantises to whatever depth is in force at that moment. At 256 colours the
 * surfaces land on the xterm grey ramp, whose rungs are 10 sRGB levels apart, so
 * window_bg #0E0F0D (#121212) and pane_bg #272822 (#262626) end up one rung
 * from each other -- 1.25:1, the "1.11:1" failure all over again. Which is why
 * these tests pin the decision table, including the two Windows cases ftxui's own
 * detection cannot see. */
void test_parse_color_depth(void)
{
    TEST("$EDGECLAW_COLOR parses truecolor / 256 / 16");

    edc::theme::ColorDepth depth = edc::theme::ColorDepth::kUnset;
    CHECK(edc::theme::ParseColorDepth("truecolor", depth) && depth == edc::theme::ColorDepth::kTrueColor,
          "truecolor not understood");
    CHECK(edc::theme::ParseColorDepth(" 24BIT ", depth) && depth == edc::theme::ColorDepth::kTrueColor,
          "a padded, upper-case 24bit not understood");
    CHECK(edc::theme::ParseColorDepth("256", depth) && depth == edc::theme::ColorDepth::kPalette256,
          "256 not understood");
    CHECK(edc::theme::ParseColorDepth("16", depth) && depth == edc::theme::ColorDepth::kPalette16,
          "16 not understood");
    CHECK(!edc::theme::ParseColorDepth("auto", depth), "\"auto\" accepted as a depth");
    CHECK(!edc::theme::ParseColorDepth("", depth), "an empty value accepted as a depth");
}

void test_detect_color_depth(void)
{
    TEST("the depth detection knows about Windows Terminal and conhost");

    using edc::theme::ColorDepth;
    const auto detect = [](const char *colorterm, const char *term, const char *term_program,
                           const char *wt_session, const char *conemu, bool windows) {
        return edc::theme::DetectColorDepth(colorterm, term, term_program, wt_session, conemu, windows);
    };

    // Windows Terminal sets neither COLORTERM nor TERM, only WT_SESSION. This is
    // the case that made every surface step invisible on Windows.
    CHECK(detect("", "", "", "8f3c9d10-...", "", false) == ColorDepth::kTrueColor, "$WT_SESSION ignored");
    CHECK(detect("", "", "", "", "ON", false) == ColorDepth::kTrueColor, "$ConEmuANSI=ON ignored");
    // A legacy conhost sets no variable at all.
    CHECK(detect("", "", "", "", "", true) == ColorDepth::kTrueColor, "conhost left at 256 colours");
    // ...but an explicit "this terminal does 256" still wins over the platform default.
    CHECK(detect("", "xterm-256color", "", "", "", true) == ColorDepth::kPalette256,
          "an explicit 256 hint was overruled by the Windows default");
    // The POSIX signals keep their meaning.
    CHECK(detect("truecolor", "", "", "", "", false) == ColorDepth::kTrueColor, "$COLORTERM ignored");
    CHECK(detect("", "screen-256color", "", "", "", false) == ColorDepth::kPalette256, "$TERM=screen ignored");
    // Nothing to go on: leave ftxui's own detection alone rather than guessing.
    CHECK(detect("", "", "", "", "", false) == ColorDepth::kUnset,
          "a POSIX shell with no hints forced a depth");
}

void test_init_color_support(void)
{
    TEST("$EDGECLAW_COLOR is installed into ftxui");

    SetEnv("EDGECLAW_COLOR", "256");
    edc::theme::InitColorSupport();
    CHECK(ftxui::Terminal::ColorSupport() == ftxui::Terminal::Color::Palette256, "256 not installed");

    SetEnv("EDGECLAW_COLOR", "truecolor");
    edc::theme::InitColorSupport();
    CHECK(ftxui::Terminal::ColorSupport() == ftxui::Terminal::Color::TrueColor, "truecolor not installed");

    SetEnv("EDGECLAW_COLOR", "");
    // Everything after this measures colours as the palette designs them.
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
}

int main(void)
{
    // Pin truecolor before anything builds a palette (the palette's statics take
    // their colours at first use). Without this, ftxui snaps every RGB to the
    // nearest entry of its 256-colour table, and the breath endpoints -- which
    // are read back as exact RGB -- would be measured against a different
    // quantisation than the one the palette was built with.
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);

    test_parse_mode();
    test_parse_osc11();
    test_light_background();
    test_palette_selection();
    test_palette_provider();
    test_light_palette_contrast();
    test_surface_separation();
    test_split_gutter();
    test_parse_color_depth();
    test_detect_color_depth();
    test_init_color_support();
    test_breath_curve();
    test_breath_endpoints();
    test_task_state_table();
#if !defined(_WIN32)
    test_query_terminal_background();
    test_init_env_override();
#endif

    if (g_failures)
    {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all theme tests passed\n");
    return 0;
}

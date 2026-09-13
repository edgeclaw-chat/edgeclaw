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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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
        {"suspended", light.suspended, "ae81ff", nullptr, 2.0}, // 2.84  constant
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

    // The surfaces come from the theme too -- window_bg is its inactiveSelection
    // and line_dim its invisibles -- and they keep the dark palette's
    // relationship: the pane is the lighter one, the window sits behind it.
    CHECK(light.window_bg == ftxui::Color::RGB(237, 237, 237), "window_bg is not Monokai Light's inactiveSelection");
    CHECK(light.line_dim == ftxui::Color::RGB(224, 224, 224), "line_dim is not Monokai Light's invisibles");
    const Rgb window = Hex("ededed");
    CHECK(Luminance(pane) > Luminance(window), "pane should be lighter than the window backdrop");
    CHECK(Contrast(pane, window) > 1.0, "pane and window collapsed into one tone");
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

} // namespace

int main(void)
{
    test_parse_mode();
    test_parse_osc11();
    test_light_background();
    test_palette_selection();
    test_light_palette_contrast();
    test_breath_curve();
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

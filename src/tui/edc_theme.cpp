#include "edc_theme.hpp"

#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <string>

#include <ftxui/screen/terminal.hpp>

#if !defined(_WIN32)
#include <chrono>
#include <poll.h>
#include <termios.h>
#endif
/* <unistd.h> is NOT guarded: this file uses STDIN_FILENO on every platform, and on Windows
 * it is satisfied by //src/common:win_compat's stand-in rather than the system header. */
#include <unistd.h>

namespace edc
{
namespace theme
{

namespace
{

Mode g_mode = Mode::kDark;

// OSC 11 with the "?" parameter, terminated by ST: "report your background
// colour". Terminals answer on the same tty, terminated by BEL (xterm,
// Terminal.app) or ST.
constexpr char kQuery[] = "\x1b]11;?\x1b\\";

// How long to wait for the reply before keeping the default. A local terminal
// answers within a millisecond or two; the timeout only exists for terminals
// that never answer at all, so startup is not held hostage by them.
constexpr int kQueryTimeoutMs = 100;

bool IsSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower >= 'a' && lower <= 'f')
        return lower - 'a' + 10;
    return -1;
}

// Decode one colour component. Terminals report either 8-bit ("1e") or 16-bit
// ("1e1e") components; both are scaled to 0..255.
bool ParseComponent(std::string_view s, unsigned char &out)
{
    if (s.empty() || s.size() > 4)
        return false;
    unsigned value = 0;
    for (char c : s)
    {
        const int digit = HexDigit(c);
        if (digit < 0)
            return false;
        value = value * 16 + static_cast<unsigned>(digit);
    }
    out = static_cast<unsigned char>(s.size() <= 2 ? value : value >> 8);
    return true;
}

std::string_view Safe(const char *value)
{
    return value ? std::string_view(value) : std::string_view();
}

bool ContainsNoCase(std::string_view hay, std::string_view needle)
{
    if (needle.empty() || hay.size() < needle.size())
        return false;
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (size_t i = 0; i + needle.size() <= hay.size(); i++)
    {
        size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j]))
            j++;
        if (j == needle.size())
            return true;
    }
    return false;
}

bool ContainsAnyNoCase(std::string_view hay, std::initializer_list<std::string_view> needles)
{
    for (std::string_view needle : needles)
        if (ContainsNoCase(hay, needle))
            return true;
    return false;
}

} // namespace

bool ParseColorDepth(std::string_view name, ColorDepth &out)
{
    size_t begin = 0;
    size_t end = name.size();
    while (begin < end && IsSpace(name[begin]))
        begin++;
    while (end > begin && IsSpace(name[end - 1]))
        end--;

    std::string lower;
    lower.reserve(end - begin);
    for (size_t i = begin; i < end; i++)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));

    if (lower == "truecolor" || lower == "24bit" || lower == "16m")
    {
        out = ColorDepth::kTrueColor;
        return true;
    }
    if (lower == "256" || lower == "palette256")
    {
        out = ColorDepth::kPalette256;
        return true;
    }
    if (lower == "16" || lower == "palette16")
    {
        out = ColorDepth::kPalette16;
        return true;
    }
    return false;
}

ColorDepth DetectColorDepth(std::string_view colorterm,
                            std::string_view term,
                            std::string_view term_program,
                            std::string_view wt_session,
                            std::string_view conemu_ansi,
                            bool windows_console)
{
    // 1. The terminal says so itself.
    if (ContainsAnyNoCase(colorterm, {"24bit", "truecolor"}))
        return ColorDepth::kTrueColor;
    if (ContainsAnyNoCase(term, {"direct", "truecolor", "kitty", "alacritty", "foot"}))
        return ColorDepth::kTrueColor;
    if (ContainsAnyNoCase(term_program,
                          {"vscode", "wezterm", "iterm", "ghostty", "warp", "apple_terminal"}))
        return ColorDepth::kTrueColor;

    // 2. Terminals that do 24-bit but do not fill in $COLORTERM.
    if (!wt_session.empty())
        return ColorDepth::kTrueColor;
    if (ContainsNoCase(conemu_ansi, "on"))
        return ColorDepth::kTrueColor;

    // 3. An explicit "256 colours only" hint beats the platform default below.
    if (ContainsNoCase(colorterm, "256") || ContainsAnyNoCase(term, {"256", "xterm", "screen", "tmux"}))
        return ColorDepth::kPalette256;

    // 4. A Windows console with VT processing on renders 24-bit SGR; $TERM is
    //    simply never set there, so this is the only signal left.
    if (windows_console && !ContainsNoCase(term, "dumb"))
        return ColorDepth::kTrueColor;

    return ColorDepth::kUnset;
}

void InitColorSupport()
{
    ColorDepth depth = ColorDepth::kUnset;
    if (const char *env = std::getenv("EDGECLAW_COLOR"))
    {
        ColorDepth parsed;
        if (ParseColorDepth(env, parsed))
            depth = parsed;
    }

#if defined(_WIN32)
    constexpr bool kWindowsConsole = true;
#else
    constexpr bool kWindowsConsole = false;
#endif

    if (depth == ColorDepth::kUnset)
        depth = DetectColorDepth(Safe(std::getenv("COLORTERM")),
                                 Safe(std::getenv("TERM")),
                                 Safe(std::getenv("TERM_PROGRAM")),
                                 Safe(std::getenv("WT_SESSION")),
                                 Safe(std::getenv("ConEmuANSI")),
                                 kWindowsConsole);

    // kUnset deliberately calls nothing: ftxui's own detection then stands.
    switch (depth)
    {
    case ColorDepth::kTrueColor:
        ::ftxui::Terminal::SetColorSupport(::ftxui::Terminal::Color::TrueColor);
        break;
    case ColorDepth::kPalette256:
        ::ftxui::Terminal::SetColorSupport(::ftxui::Terminal::Color::Palette256);
        break;
    case ColorDepth::kPalette16:
        ::ftxui::Terminal::SetColorSupport(::ftxui::Terminal::Color::Palette16);
        break;
    case ColorDepth::kUnset:
        break;
    }
}

bool ParseMode(std::string_view name, Mode &out)
{
    size_t begin = 0;
    size_t end = name.size();
    while (begin < end && IsSpace(name[begin]))
        begin++;
    while (end > begin && IsSpace(name[end - 1]))
        end--;

    std::string lower;
    lower.reserve(end - begin);
    for (size_t i = begin; i < end; i++)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));

    if (lower == "dark")
    {
        out = Mode::kDark;
        return true;
    }
    if (lower == "light")
    {
        out = Mode::kLight;
        return true;
    }
    return false;
}

void SetMode(Mode mode)
{
    g_mode = mode;
}

bool IsLight()
{
    return g_mode == Mode::kLight;
}

bool ParseOsc11(std::string_view buffer, unsigned char &r, unsigned char &g, unsigned char &b)
{
    constexpr size_t kPrefix = 4; // "]11;"
    const size_t start = buffer.find("]11;");
    if (start == std::string_view::npos)
        return false;

    // The reply ends at BEL or at ST, whichever comes first. Neither present
    // yet means it is still arriving.
    const size_t payload_begin = start + kPrefix;
    size_t payload_end = buffer.find('\x07', payload_begin);
    const size_t st = buffer.find("\x1b\\", payload_begin);
    if (st != std::string_view::npos && (payload_end == std::string_view::npos || st < payload_end))
        payload_end = st;
    if (payload_end == std::string_view::npos)
        return false;

    const std::string_view payload = buffer.substr(payload_begin, payload_end - payload_begin);

    if (payload.rfind("rgb:", 0) == 0)
    {
        const std::string_view rest = payload.substr(4);
        const size_t slash1 = rest.find('/');
        if (slash1 == std::string_view::npos)
            return false;
        const size_t slash2 = rest.find('/', slash1 + 1);
        if (slash2 == std::string_view::npos)
            return false;
        return ParseComponent(rest.substr(0, slash1), r) &&
               ParseComponent(rest.substr(slash1 + 1, slash2 - slash1 - 1), g) &&
               ParseComponent(rest.substr(slash2 + 1), b);
    }

    if (!payload.empty() && payload[0] == '#')
    {
        const std::string_view hex = payload.substr(1);
        if (hex.size() == 6)
            return ParseComponent(hex.substr(0, 2), r) && ParseComponent(hex.substr(2, 2), g) &&
                   ParseComponent(hex.substr(4, 2), b);
        if (hex.size() == 3)
            // #rgb: each nibble is doubled, so "f" means 0xff.
            return ParseComponent(std::string(2, hex[0]), r) && ParseComponent(std::string(2, hex[1]), g) &&
                   ParseComponent(std::string(2, hex[2]), b);
    }

    return false;
}

bool IsLightBackground(unsigned char r, unsigned char g, unsigned char b)
{
    // ITU-R BT.601 luma, scaled to 0..255. The midpoint splits the common
    // themes cleanly: #1e1e1e and #272822 land near 30, an off-white near 250.
    const int luma = (299 * int(r) + 587 * int(g) + 114 * int(b)) / 1000;
    return luma > 127;
}

#if defined(_WIN32)

bool QueryTerminalBackground(int, int, unsigned char &, unsigned char &, unsigned char &)
{
    return false; // no OSC 11 probe on Windows: the dark default stands.
}

#else

bool QueryTerminalBackground(int fd, int timeout_ms, unsigned char &r, unsigned char &g, unsigned char &b)
{
    termios saved{};
    if (tcgetattr(fd, &saved) != 0)
        return false; // not a tty (a pipe, a file, CI) -- nothing to ask

    termios raw = saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0; // poll() drives the wait, reads never block
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) != 0)
        return false;

    bool answered = false;
    if (write(fd, kQuery, sizeof(kQuery) - 1) == static_cast<ssize_t>(sizeof(kQuery) - 1))
    {
        std::string buffer;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true)
        {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            if (left <= 0)
                break;

            pollfd wait{fd, POLLIN, 0};
            if (poll(&wait, 1, static_cast<int>(left)) <= 0)
                break; // timed out (or the tty went away)

            char chunk[64];
            const ssize_t n = read(fd, chunk, sizeof(chunk));
            if (n <= 0)
                break;
            buffer.append(chunk, static_cast<size_t>(n));
            if (ParseOsc11(buffer, r, g, b))
            {
                answered = true;
                break;
            }
        }
    }

    // Hand the tty back exactly as it was: ftxui sets up its own raw mode when
    // the app starts, and must see the state it expects.
    tcsetattr(fd, TCSANOW, &saved);
    return answered;
}

#endif

void Init()
{
    // An explicit request wins: no point asking a terminal we have been told
    // to ignore.
    if (const char *env = std::getenv("EDGECLAW_THEME"))
    {
        Mode parsed;
        if (ParseMode(env, parsed))
        {
            SetMode(parsed);
            return;
        }
    }

    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
    if (!QueryTerminalBackground(STDIN_FILENO, kQueryTimeoutMs, r, g, b))
        return; // silence (or no tty): keep the dark default

    SetMode(IsLightBackground(r, g, b) ? Mode::kLight : Mode::kDark);
}

} // namespace theme
} // namespace edc

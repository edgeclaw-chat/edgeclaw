#include "edc_colorscheme_palette.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace edc::theme
{

namespace
{

using ::ftxui::Color;

Color FromRgb(uint32_t rgb)
{
    return Color::RGB(static_cast<uint8_t>((rgb >> 16) & 0xff), static_cast<uint8_t>((rgb >> 8) & 0xff),
                      static_cast<uint8_t>(rgb & 0xff));
}

/* All the repair maths runs on raw 0xRRGGBB values. This ftxui build keeps a
 * Color's channels private with no getter, so luminance cannot be read back
 * from a Color -- but every value we need to reason about comes from the parser
 * as raw RGB anyway. */

float Channel(uint8_t v)
{
    const double s = v / 255.0;
    return static_cast<float>(s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4));
}

float Luma(uint32_t rgb)
{
    const uint8_t r = (rgb >> 16) & 0xff;
    const uint8_t g = (rgb >> 8) & 0xff;
    const uint8_t b = rgb & 0xff;
    return 0.2126f * Channel(r) + 0.7152f * Channel(g) + 0.0722f * Channel(b);
}

float Contrast(uint32_t a, uint32_t b)
{
    const float la = Luma(a);
    const float lb = Luma(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

/* Scale each channel by |factor|, clamped, keeping the hue. */
uint32_t Scale(uint32_t rgb, float factor)
{
    uint8_t out[3];
    for (int i = 0; i < 3; i++)
    {
        const uint8_t v = (rgb >> (16 - 8 * i)) & 0xff;
        out[i] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v * factor)));
    }
    return ((uint32_t)out[0] << 16) | ((uint32_t)out[1] << 8) | (uint32_t)out[2];
}

/* Linear mix of two raw colours, t in [0,1]. t=0 -> a, t=1 -> b. */
uint32_t Mix(uint32_t a, uint32_t b, float t)
{
    uint8_t out[3];
    for (int i = 0; i < 3; i++)
    {
        const int av = (a >> (16 - 8 * i)) & 0xff;
        const int bv = (b >> (16 - 8 * i)) & 0xff;
        out[i] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, av + (bv - av) * t)));
    }
    return ((uint32_t)out[0] << 16) | ((uint32_t)out[1] << 8) | (uint32_t)out[2];
}

} // namespace

style::Palette PaletteFromScheme(const edc_colorscheme &cs, const style::Palette &base, bool base_light)
{
    (void)base_light; // the fallback palette is passed explicitly; kept for call-site clarity
    style::Palette p = base;

    auto apply = [&](int role, Color style::Palette::*slot) {
        if (cs.set[role])
            p.*slot = FromRgb(cs.rgb[role]);
    };

    apply(EDC_CS_WINDOW_BG, &style::Palette::window_bg);
    apply(EDC_CS_PANE_BG, &style::Palette::pane_bg);
    apply(EDC_CS_TEXT_DIM, &style::Palette::text_dim);
    apply(EDC_CS_TEXT, &style::Palette::text);
    apply(EDC_CS_LINE_DIM, &style::Palette::line_dim);
    apply(EDC_CS_DIALOG_BG, &style::Palette::dialog_bg);
    apply(EDC_CS_ACCENT, &style::Palette::accent);
    apply(EDC_CS_OK, &style::Palette::ok);
    apply(EDC_CS_WARN, &style::Palette::warn);
    apply(EDC_CS_ERROR, &style::Palette::error);
    apply(EDC_CS_INTERRUPTED, &style::Palette::interrupted);
    apply(EDC_CS_THOUGHT_OPEN, &style::Palette::thought_open);
    apply(EDC_CS_THOUGHT_CLOSED, &style::Palette::thought_closed);
    apply(EDC_CS_PROMPT, &style::Palette::prompt);
    apply(EDC_CS_USER_RULE, &style::Palette::user_rule);
    apply(EDC_CS_MD_H1, &style::Palette::md_h1);
    apply(EDC_CS_MD_H1_BG, &style::Palette::md_h1_bg);
    apply(EDC_CS_MD_H2, &style::Palette::md_h2);
    apply(EDC_CS_MD_H3, &style::Palette::md_h3);
    apply(EDC_CS_MD_H4, &style::Palette::md_h4);
    apply(EDC_CS_MD_H5, &style::Palette::md_h5);
    apply(EDC_CS_MD_H6, &style::Palette::md_h6);
    apply(EDC_CS_MD_CODE, &style::Palette::md_code);
    apply(EDC_CS_MD_CODE_INLINE, &style::Palette::md_code_inline);
    apply(EDC_CS_MD_CODE_BG, &style::Palette::md_code_bg);
    apply(EDC_CS_MD_RULE, &style::Palette::md_rule);
    apply(EDC_CS_MD_QUOTE, &style::Palette::md_quote);
    apply(EDC_CS_MD_LIST, &style::Palette::md_list);
    apply(EDC_CS_MD_LINK, &style::Palette::md_link);
    apply(EDC_CS_MD_TABLE_HEAD, &style::Palette::md_table_head);
    apply(EDC_CS_PASTE_TEXT, &style::Palette::paste_text);
    apply(EDC_CS_PASTE_SURFACE, &style::Palette::paste_surface);
    apply(EDC_CS_MENTION_TEXT, &style::Palette::mention_text);

    /* The Thought toggle's two states must be the SAME hue at two brightnesses:
     * collapsed = a dimmed version of the expanded theme colour. The built-in
     * palettes do this by hand (yellow 8a8345 -> a89a5e). An imported theme's
     * `thought_closed` binding (Comment) is usually grey and unrelated to its
     * `thought_open` (Todo), so it reads as "grey until expanded". Re-derive the
     * collapsed colour from the expanded one toward the pane, keeping the hue and
     * guaranteeing the collapsed state is still recognisably the theme. The fade is
     * deliberately small: the collapsed toggle must stay clearly readable, only a
     * step dimmer than the expanded colour -- not washed out toward a dark pane. */
    if (cs.set[EDC_CS_THOUGHT_OPEN])
    {
        const uint32_t open = cs.rgb[EDC_CS_THOUGHT_OPEN];
        const uint32_t toward = cs.set[EDC_CS_PANE_BG] ? cs.rgb[EDC_CS_PANE_BG] : 0x000000u;
        p.thought_closed = FromRgb(Mix(open, toward, 0.25f));
    }

    /* The borderless split needs a visible step between window_bg and pane_bg.
     * Imported themes set both from Normal's bg, so they arrive identical: derive
     * a distinct window tone from the pane's raw RGB. We only touch this when
     * BOTH roles came from the theme (otherwise the built-in pair already has
     * the separation designed in). */
    if (cs.set[EDC_CS_WINDOW_BG] && cs.set[EDC_CS_PANE_BG] &&
        Contrast(cs.rgb[EDC_CS_WINDOW_BG], cs.rgb[EDC_CS_PANE_BG]) < style::kMinSurfaceSeparation)
    {
        const uint32_t pane = cs.rgb[EDC_CS_PANE_BG];
        const bool pane_light = Luma(pane) > 0.18f;
        p.window_bg = FromRgb(Scale(pane, pane_light ? 0.72f : 1.45f));
    }

    /* A pasted chip needs its own surface, distinct from the pane beneath it. */
    if (cs.set[EDC_CS_PASTE_SURFACE] && cs.set[EDC_CS_PANE_BG] &&
        Contrast(cs.rgb[EDC_CS_PASTE_SURFACE], cs.rgb[EDC_CS_PANE_BG]) < 1.12f)
    {
        const uint32_t pane = cs.rgb[EDC_CS_PANE_BG];
        const bool light = Luma(pane) > 0.18f;
        p.paste_surface = FromRgb(Scale(pane, light ? 0.85f : 1.25f));
    }

    /* The h1 banner background: a theme that defines markdownH1's bg gets it verbatim
     * (apply() above). Most do not (cyberdream included), and the built-in fallback is
     * Monokai's hot pink #f92672, which clashes badly with an imported palette. Derive
     * a banner from the pane surface instead: a subtle raised tone a step away from the
     * pane, always on the same side of light/dark. The h1 text colour is applied on top. */
    if (!cs.set[EDC_CS_MD_H1_BG] && cs.set[EDC_CS_PANE_BG])
    {
        const uint32_t pane = cs.rgb[EDC_CS_PANE_BG];
        const bool light = Luma(pane) > 0.18f;
        p.md_h1_bg = FromRgb(Scale(pane, light ? 0.90f : 1.30f));
    }

    return p;
}

std::shared_ptr<FixedSchemePalette> InstallScheme(const std::string &edc_home, const std::string &name)
{
    if (name.empty() || name == "off" || name == "builtin")
        return nullptr;

    edc_colorscheme cs;
    if (!edc_colorscheme_load(edc_home.c_str(), name.c_str(), &cs))
        return nullptr;

    // Decide the fallback from the imported pane: a dark theme borrows the
    // dark built-in for groups it does not mention, and vice versa.
    const bool light = cs.set[EDC_CS_PANE_BG] && Luma(cs.rgb[EDC_CS_PANE_BG]) > 0.18f;
    const style::Palette &base = light ? style::light_palette() : style::dark_palette();
    auto provider = std::make_shared<FixedSchemePalette>(PaletteFromScheme(cs, base, light));
    style::SetPaletteProvider(provider);
    return provider;
}

void ClearScheme()
{
    style::SetPaletteProvider(nullptr);
}

} // namespace edc::theme

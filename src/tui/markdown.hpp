#pragma once
#include <ftxui/dom/elements.hpp>
#include <string_view>

namespace edc
{

// Render markdown to an ftxui element tree. |wrap_cols| is the width the caller will lay
// the tree out at, or 0 when unknown: ftxui's flows only break *between* their children,
// so an unbroken run (CJK text, a long URL or path) would be one child that overflows and
// gets clipped; knowing the width lets it be hard-broken between code points instead
// (see State::AddWord).
ftxui::Element RenderMarkdown(std::string_view md, int wrap_cols = 0);

// Render a plain code snippet with tree-sitter syntax colours. |lang| is a
// registered language tag; when empty/unknown the text is returned unhighlighted
// in the plain code colour. Used by fenced blocks and by plain tool output
// (read_file) alike, so both share one painting path.
ftxui::Element HighlightedCode(std::string_view code, const char *lang);

} // namespace edc

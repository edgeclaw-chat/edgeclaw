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

} // namespace edc

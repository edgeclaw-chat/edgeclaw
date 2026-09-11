#pragma once
#include <ftxui/dom/elements.hpp>
#include <string_view>

namespace edc
{

// Render markdown to an ftxui element tree.
//
// |wrap_cols| is the width the caller will lay the tree out at, or 0 when it
// is unknown. It is needed because ftxui's flow containers only break *between*
// their children: a paragraph is a flow of per-word elements, so an unbroken
// run (CJK text, a long URL or path) is a single child that cannot be broken,
// overflows its box and gets clipped. Knowing the width lets such a run be
// hard-broken between code points instead (see State::AddWord).
ftxui::Element RenderMarkdown(std::string_view md, int wrap_cols = 0);

} // namespace edc

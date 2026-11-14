#pragma once

// View: drawing a modal over the pane, so that its rectangle survives contact with the text.
//
// Two things make a borderless modal read as a rectangle, and neither is about the panel:
//
//  1. the ground behind it must be ONE colour. The chat alternates between the message surface
//     and the gap between messages, and `dim` darkens those by different amounts, so a straight
//     edge next to them looked jagged;
//  2. no FULLWIDTH character may sit in the cell just left of the panel. A CJK glyph starting
//     there occupies two columns, and ftxui's Screen::ToString() skips the cell after a wide
//     character -- so the panel's first column was not merely covered, it was never emitted,
//     shortening that row by a column and shifting the whole panel.
//
// Both are handled here rather than by a border: a border would add a frame the interface does
// not use anywhere else, and its own outer edge would meet the same uneven ground.

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

namespace edc
{

/* Draw |backdrop|, flatten its background to |ground|, then draw |panel| centred on top.
 *
 * |panel_width| is the panel's rendered width in columns, |panel_bg| the colour that identifies
 * its surface. Both are needed to know where its edge lands without asking ftxui where the
 * centring put it: the panel is placed with explicit padding computed from the box width, so
 * the first column is arithmetic rather than a guess.
 *
 * |backdrop| stays legible underneath (the caller dims it); only backgrounds are replaced. */
::ftxui::Element ModalOverlay(::ftxui::Element backdrop, ::ftxui::Element panel, int panel_width,
                              ::ftxui::Color ground, ::ftxui::Color panel_bg);

} // namespace edc

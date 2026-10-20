#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

// How a user-typed line marks the two things that are more than words: an `@file`
// reference and a folded paste block.
//
// This is the DISPLAY side of a rule whose semantics live elsewhere:
//   - `@path` is read by the mention pass (edc_pass_mentions.c) and offered by the `@`
//     completion (CommandPalette::ParseMention). Both use the same rule, restated here:
//     `@` at the start of the line or right after whitespace, the token runs to the next
//     whitespace, trailing ASCII punctuation is not part of it, and `\@` is an escape.
//   - `[Pasted #<n> · <m> line(s)]` is the label the input editor writes for a folded
//     paste (TUIWindow::CommitPasteBuffer).
//
// The bubble and the input line must agree, or a line that was coloured as a reference
// in the box turns into plain text once sent, and the user is left wondering whether it
// was treated as one at all. So the styling itself is also shared: StyleToken() is what
// both call, and its colours are the input box's knobs in edc_style.hpp.
namespace edc
{

struct TokenSpan
{
    size_t begin = 0;
    size_t len = 0;
    bool paste = false; // false = an `@file` mention
};

// Every token in |text|, in order, non-overlapping. A token the scanner cannot finish
// (an `@` at the very end of a line, an unterminated label) is left as plain text - the
// same way the pass would ignore it.
std::vector<TokenSpan> ScanTokens(const std::string &text);

// |payload_json| = a folded-paste map (placeholder -> bytes), as stored with a message.
// Returns |text| with each block's bytes put back under its placeholder: the reverse of
// what the app does before classifying, and what a display needs when the only text it has
// is the expanded one (the task's user message). Non-JSON or empty = |text| unchanged.
std::string FoldPastedText(std::string text, const std::string &payload_json);

// The one place a token's look is decided: a paste block is a chip (pasted colour on its
// own surface), a mention wears the keyword colour with no surface. No palette slot is
// spent -- these are content colours, see edc_style.hpp.
::ftxui::Element StyleToken(::ftxui::Element element, bool paste);

} // namespace edc

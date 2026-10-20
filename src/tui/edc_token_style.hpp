#pragma once

#include <ftxui/dom/elements.hpp>
#include <functional>
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
    // The token is FINISHED: nothing that could still extend it follows. A mention being
    // typed is not marked (it is not a reference yet), and neither is one the caret can
    // still grow; see MentionIsMarked().
    bool closed = false;
};

// Every token in |text|, in order, non-overlapping. A token the scanner cannot finish
// (an `@` at the very end of a line, an unterminated label) is left as plain text - the
// same way the pass would ignore it.
//
// |end_closes|: whether a token that reaches the end of the text counts as finished. The
// input box passes false -- the caret sitting there means it is still being typed -- and a
// bubble (a sent line, no caret) passes true.
std::vector<TokenSpan> ScanTokens(const std::string &text, bool end_closes = true);

// The filesystem path behind a mention, without the leading `@` and without a line range:
// "@a.c#12-20" -> "a.c". The same split the pass makes (a '#' that does not open a range
// is part of the name).
std::string MentionPath(const std::string &token_text);

// How a mention's path is checked, installed by the frontend that owns the app (the
// display must not touch the disk: it renders every frame). TUIWindow points this at the
// app, i.e. the mention pass's own resolution (session directory + sandbox); a test
// installs its own. No check installed = nothing is "real", so nothing is marked.
using MentionPathCheck = std::function<bool(const std::string &)>;
void SetMentionPathCheck(MentionPathCheck check);

// Whether a token wears its mark. A paste block always does; a mention only when it is
// FINISHED and the path EXISTS -- the colour means "this reference is real", so painting a
// half-typed or non-existent one would be lying.
bool MentionIsMarked(const TokenSpan &span, const std::string &token_text);

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

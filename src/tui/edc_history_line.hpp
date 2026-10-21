#pragma once

#include <string>

// One recalled input line, on disk: `<mid>\t<text>`.
//
// The mid (the id of the message that line was submitted as) is what a recalled folded
// paste uses to ask the store for its bytes back, so it has to survive a restart next to
// the text. Two properties matter and are both easy to get wrong:
//
//  - the text is user input: it may contain newlines (Alt+Enter) and tabs (a pasted TSV
//    line), so it is escaped -- the file stays one line per entry and the first tab is
//    unambiguous;
//  - files written before mids existed have no tab at all, and must still load.
namespace edc
{

struct HistoryLine
{
    std::string mid;  // empty = no mid (an entry from before the field existed)
    std::string text; // the line as it goes back into the input box
};

// |mid| empty = write the text alone (the old shape, still readable).
std::string EncodeHistoryLine(const std::string &mid, const std::string &text);

// A line with no leading mid is read as an old entry. A mid is only recognised when it
// looks like one we generate (`<session>#<n>#<ms>`), so a pasted line whose own text
// starts with something tab-separated is never mistaken for one.
HistoryLine DecodeHistoryLine(const std::string &line);

// True for a mid this frontend could have generated.
bool LooksLikeMid(const std::string &candidate);

} // namespace edc

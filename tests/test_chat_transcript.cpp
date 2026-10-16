// Tests for the TUI's chat transcript codec (edc::EncodeTranscriptLine /
// DecodeTranscriptLine / LoadTranscript).
//
// The transcript is the view's own record — one line per bubble that was shown —
// and it is the only thing the TUI replays on startup. Two properties matter and
// are pinned here: a bubble must survive the round trip byte for byte (a reply
// containing tabs, newlines or backslashes must not come back mangled or split
// into two bubbles), and the reader must tolerate a file that a crash or an older
// build left in a different shape (a partial tail line, a line with too few
// columns) by skipping exactly that line instead of losing the whole log.
#include "edc_chat_transcript.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    if (ok)
    {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    g_failures++;
}

edc::ChatTranscriptEntry MakeEntry(const std::string &kind, int render, const std::string &task_id,
                                   const std::string &header, const std::string &content)
{
    edc::ChatTranscriptEntry e;
    e.kind = kind;
    e.render_pref = render;
    e.task_id = task_id;
    e.header = header;
    e.content = content;
    return e;
}

bool Same(const edc::ChatTranscriptEntry &a, const edc::ChatTranscriptEntry &b)
{
    return a.kind == b.kind && a.render_pref == b.render_pref && a.task_id == b.task_id && a.header == b.header &&
           a.content == b.content;
}

void TestRoundTrip()
{
    std::cout << "a bubble survives the round trip byte for byte\n";

    const edc::ChatTranscriptEntry cases[] = {
        MakeEntry("user", 0, "", "", "hello"),
        MakeEntry("reply", 1, "277", "", "line one\nline two\n\nline four"),
        MakeEntry("tool", 0, "277", "git status --short", " M src/tui/edc_tui_window.cpp"),
        /* every escaped character at once, plus escaping of the escape itself */
        MakeEntry("sys", 0, "", "", "tab\there \\\\ backslash \\n literal"),
        MakeEntry("thought", 0, "3", "", "ünïcödé 中文 🚀"),
        MakeEntry("tool", 1, "9", "todowrite", "# TODO\n\n1/2 completed"),
        MakeEntry("reply", 1, "", "", ""), /* empty content: still one bubble */
    };

    for (const auto &in : cases)
    {
        edc::ChatTranscriptEntry out;
        const bool decoded = edc::DecodeTranscriptLine(edc::EncodeTranscriptLine(in), &out);
        Check(decoded, "line decodes: " + in.kind);
        Check(decoded && Same(in, out), "fields identical: " + in.kind + " / " + in.header);
    }

    /* A raw tab inside the last field must not split the line into two bubbles. */
    const std::string awkward =
        "reply\t1\t7\t\ttab\there\n";
    edc::ChatTranscriptEntry out;
    Check(edc::DecodeTranscriptLine(awkward, &out) && out.content == "tab\there", "a raw tab in content is kept");
    Check(out.kind == "reply" && out.render_pref == 1 && out.task_id == "7", "columns before it still parse");
}

void TestRejects()
{
    std::cout << "lines that are not a bubble are rejected, not guessed\n";
    edc::ChatTranscriptEntry out;

    Check(!edc::DecodeTranscriptLine("", &out), "empty line");
    Check(!edc::DecodeTranscriptLine("\n", &out), "blank line");
    Check(!edc::DecodeTranscriptLine("user\t0", &out), "truncated: two columns only");
    Check(!edc::DecodeTranscriptLine("user\t0\t5\theader", &out), "truncated: four columns only (a crash mid-append)");
    Check(!edc::DecodeTranscriptLine("\t0\t5\t\t", &out), "empty kind");
    Check(edc::DecodeTranscriptLine("user\t0\t\t\tX\r\n", &out), "CRLF line still decodes");
}

void TestLegacyShapes()
{
    std::cout << "the shapes older builds wrote stay readable\n";
    edc::ChatTranscriptEntry out;

    /* Written before a task existed: empty task_id column (see s7's transcript). */
    Check(edc::DecodeTranscriptLine("user\t0\t\t\t/session list", &out) && out.kind == "user" &&
              out.render_pref == 0 && out.task_id.empty() && out.content == "/session list",
          "no task id, slash command");

    /* A render column that is not a number falls back to PLAIN rather than
     * shifting the fields. */
    Check(edc::DecodeTranscriptLine("reply\tplain\t9\t\ttext", &out) && out.render_pref == 0 &&
              out.content == "text",
          "non-numeric render column");
}

void TestLoadTranscript()
{
    std::cout << "loading a file skips the bad lines and keeps the good ones\n";

    const std::string path = "/tmp/test_chat_transcript_chat.jsonl";
    const std::string good1 = edc::EncodeTranscriptLine(MakeEntry("user", 0, "", "", "first"));
    const std::string good2 = edc::EncodeTranscriptLine(MakeEntry("reply", 1, "42", "", "second"));
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << good1;
        f << "not a transcript line at all\n";
        f << "reply\t1\t42\n"; /* too few columns: written by nothing we ship */
        f << good2;
        f << "reply\t1\t42\t\tpartial tail the crash cut here"; /* no newline */
    }

    std::vector<edc::ChatTranscriptEntry> got;
    const size_t n = edc::LoadTranscript(path, [&got](const edc::ChatTranscriptEntry &e) { got.push_back(e); });
    Check(n == 2, "two whole lines loaded (got " + std::to_string(n) + ")");
    Check(got.size() == 2 && got[0].content == "first" && got[1].content == "second", "loaded in file order");
    Check(got.size() == 2 && got[1].render_pref == 1, "render column of an intact line survives");
    Check(edc::TranscriptEndsMidLine(path), "the unterminated tail is reported (append must not land on it)");

    std::remove(path.c_str());
    Check(edc::LoadTranscript(path, [](const edc::ChatTranscriptEntry &) {}) == 0, "missing file: no entries");
    Check(!edc::TranscriptEndsMidLine(path), "missing file does not look mid-line");

    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
    }
    Check(edc::LoadTranscript(path, [](const edc::ChatTranscriptEntry &) {}) == 0, "empty file: no entries");
    Check(!edc::TranscriptEndsMidLine(path), "empty file does not look mid-line");
    std::remove(path.c_str());

    Check(edc::LoadTranscript("", [](const edc::ChatTranscriptEntry &) {}) == 0, "empty path: no entries");

    /* A file with no newline at all is one unfinished append: nothing to load. */
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "user\t0\t\t\ttyped but never finished";
    }
    Check(edc::LoadTranscript(path, [](const edc::ChatTranscriptEntry &) {}) == 0, "no terminated line: no entries");
    std::remove(path.c_str());
}

// Progressive replay: the startup path reads only the file's tail, then decodes the
// older prefix (possibly cancelled). The two halves must reconstruct the whole log.
void TestProgressiveReplay()
{
    std::cout << "progressive replay: tail + prefix rebuild the log; cancel stops early\n";

    const std::string path = "/tmp/test_chat_transcript_progressive.jsonl";
    const int total = 1000;
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < total; i++)
            f << "user\t0\t\t\tline" << i << "\n";
        f << "user\t0\t\t\tpartial tail"; /* no newline: an unfinished append, dropped */
    }

    std::vector<edc::ChatTranscriptEntry> full;
    edc::LoadTranscript(path, [&full](const edc::ChatTranscriptEntry &e) { full.push_back(e); });
    Check(full.size() == static_cast<size_t>(total), "full replay has every whole line");

    size_t prefix_end = 0;
    std::vector<edc::ChatTranscriptEntry> tail;
    const size_t n_tail = edc::LoadTranscriptTail(path, 100, &prefix_end, &tail);
    Check(n_tail == 100, "tail replay returns the newest 100 (got " + std::to_string(n_tail) + ")");
    Check(prefix_end > 0, "tail replay reports where the older history ends");
    Check(tail.size() == 100 && tail.front().content == "line900" && tail.back().content == "line999",
          "the tail is the newest end, in file order");

    std::vector<edc::ChatTranscriptEntry> prefix;
    const size_t n_prefix = edc::LoadTranscriptPrefix(path, prefix_end, &prefix);
    Check(n_prefix == 900, "prefix replay returns the older 900 (got " + std::to_string(n_prefix) + ")");
    bool same = prefix.size() + tail.size() == full.size();
    for (size_t i = 0; same && i < prefix.size(); i++)
        same = Same(prefix[i], full[i]);
    for (size_t i = 0; same && i < tail.size(); i++)
        same = Same(tail[i], full[prefix.size() + i]);
    Check(same, "prefix + tail reproduce the full replay element for element");

    /* A cancel that fires on the first poll stops early (the poll runs every ~256
     * lines), so a session switch never waits for a whole history to decode. */
    int calls = 0;
    std::vector<edc::ChatTranscriptEntry> stopped;
    const size_t n_stopped = edc::LoadTranscriptPrefix(path, prefix_end, &stopped, [&calls]() { return ++calls > 0; });
    Check(n_stopped > 0 && n_stopped < static_cast<size_t>(total),
          "cancel stops the decode early (got " + std::to_string(n_stopped) + ")");
    Check(stopped.size() == n_stopped, "the entries decoded before the cancel are kept");

    std::vector<edc::ChatTranscriptEntry> never;
    const size_t n_never = edc::LoadTranscriptPrefix(path, prefix_end, &never, []() { return false; });
    Check(n_never == 900, "a never-cancelled decode returns the whole prefix");

    /* Fewer lines than the tail size: the whole file is the tail, nothing to
     * backfill (the caller then starts no worker and shows no hint). */
    const std::string small = path + ".small";
    {
        std::ofstream f(small, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 5; i++)
            f << "user\t0\t\t\tsmall" << i << "\n";
    }
    size_t small_end = 123;
    std::vector<edc::ChatTranscriptEntry> small_tail;
    const size_t n_small = edc::LoadTranscriptTail(small, 200, &small_end, &small_tail);
    Check(n_small == 5 && small_tail.size() == 5, "a log shorter than the tail is all tail");
    Check(small_end == 0, "a short log reports no older history (so no backfill/hint)");
    std::remove(small.c_str());

    std::remove(path.c_str());
}

} // namespace

int main()
{
    std::cout << "chat transcript (TUI-owned log)\n";
    TestRoundTrip();
    TestRejects();
    TestLegacyShapes();
    TestLoadTranscript();
    TestProgressiveReplay();

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}

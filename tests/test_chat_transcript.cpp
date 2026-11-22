// Tests for the chat_bubbles row codec (edc::EncodeTranscriptLine /
// DecodeTranscriptLine). One bubble is one escaped, tab-separated line stored
// verbatim in chat_bubbles; there is no transcript file any more.
//
// The property pinned here: a bubble must survive the round trip byte for byte
// (a reply containing tabs, newlines or backslashes must not come back mangled
// or split into two lines), and lines that are not bubbles are rejected rather
// than guessed.
#include "edc_chat_transcript.hpp"
#include <iostream>
#include <string>

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
    const std::string awkward = "reply\t1\t7\t\ttab\there\n";
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
    Check(!edc::DecodeTranscriptLine("user\t0\t5\theader", &out), "truncated: four columns only");
    Check(!edc::DecodeTranscriptLine("\t0\t5\t\t", &out), "empty kind");
    Check(edc::DecodeTranscriptLine("user\t0\t\t\tX\r\n", &out), "CRLF line still decodes");
}

void TestLegacyShapes()
{
    std::cout << "the shapes older builds wrote stay readable\n";
    edc::ChatTranscriptEntry out;

    Check(edc::DecodeTranscriptLine("user\t0\t\t\t/session list", &out) && out.kind == "user" &&
              out.render_pref == 0 && out.task_id.empty() && out.content == "/session list",
          "no task id, slash command");

    Check(edc::DecodeTranscriptLine("reply\tplain\t9\t\ttext", &out) && out.render_pref == 0 &&
              out.content == "text",
          "non-numeric render column");
}

} // namespace

int main()
{
    std::cout << "chat_bubbles row codec\n";
    TestRoundTrip();
    TestRejects();
    TestLegacyShapes();

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}

#include "edc_transcript_store.hpp"

namespace edc::port
{

TranscriptStore::TranscriptStore(IAppFacade &app) : app_(app)
{
}

bool TranscriptStore::PathFor(const std::string &session_id, std::string *out) const
{
    return app_.ChatLogPathFor(session_id, out);
}

size_t TranscriptStore::Replay(const std::string &path,
                               const std::function<void(const ChatTranscriptEntry &)> &emit) const
{
    return LoadTranscript(path, emit);
}

void TranscriptStore::Append(const std::string &path, const ChatTranscriptEntry &entry)
{
    if (path.empty())
        return;

    // A session's directory only exists once the session has been saved, and a
    // command-only session (e.g. /help) may never be saved before its first line
    // is displayed — make sure the parent is there before appending.
    app_.EnsureParentDir(path);

    // A previous process may have died mid-append, leaving a fragment without its
    // newline. Re-terminate it first: otherwise this line lands on top of it and
    // the two bubbles merge into one undecodable line.
    std::string line = EncodeTranscriptLine(entry);
    if (TranscriptEndsMidLine(path))
        line.insert(line.begin(), '\n');

    // Symlink-safe append: the transcript lives under the model-writable workspace,
    // so refuse to follow a planted symlink (see edc_file_fopen_safe /
    // edc_app_write_file).
    app_.WriteFile(path, line.data(), line.size(), /*append=*/true, /*under_workspace=*/true);
}

} // namespace edc::port

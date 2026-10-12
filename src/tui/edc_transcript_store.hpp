#pragma once

// Port: the session transcript store (TranscriptStore).
//
// A transcript is the TUI's own record of what it put on screen (one bubble per
// line). This I/O used to live in TUIWindow (path resolution, parent dir, fixing
// a half-written line, symlink-safe append, replay). It is now a store object:
//   * TUIWindow (and its replay callback) only turns entries into on-screen
//     bubbles, it no longer knows the file format;
//   * the file format (EncodeTranscriptLine / LoadTranscript /
//     TranscriptEndsMidLine) is centralized here and testable alone;
//   * paths are resolved through IAppFacade (which knows the workspace), so the
//     store does not know the app's concrete type.

#include "edc_app_facade.hpp"      /* port::IAppFacade */
#include "edc_chat_transcript.hpp" /* edc::ChatTranscriptEntry */

#include <functional>
#include <string>

namespace edc::port
{

class ITranscriptStore
{
  public:
    virtual ~ITranscriptStore() = default;

    // Resolve the transcript path by session id (<workspace>/<session_id>/chat.jsonl).
    virtual bool PathFor(const std::string &session_id, std::string *out) const = 0;

    // Read-only replay: hands every decodable line to |emit|, returns the count
    // (0 = no usable transcript).
    virtual size_t Replay(const std::string &path,
                          const std::function<void(const ChatTranscriptEntry &)> &emit) const = 0;

    // Append one entry (after ensuring the parent dir, fixing a possible
    // half-written line, symlink-safe).
    virtual void Append(const std::string &path, const ChatTranscriptEntry &entry) = 0;
};

// Production implementation backed by IAppFacade.
class TranscriptStore : public ITranscriptStore
{
  public:
    explicit TranscriptStore(IAppFacade &app);

    bool PathFor(const std::string &session_id, std::string *out) const override;
    size_t Replay(const std::string &path,
                  const std::function<void(const ChatTranscriptEntry &)> &emit) const override;
    void Append(const std::string &path, const ChatTranscriptEntry &entry) override;

  private:
    IAppFacade &app_;
};

} // namespace edc::port

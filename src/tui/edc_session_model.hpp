#pragma once

#include <string>
#include <utility>

namespace edc::model
{

// The frontend's "which session is being displayed" state.
//
// These three values (displayed session id / its bound workdir / its transcript
// path) used to be bare TUIWindow members read and written in several places
// (submit, footer, switch, transcript). They now live in a model, giving the
// displayed identity a single owner and letting the dispatcher read it to judge
// ownership.
class SessionModel
{
  public:
    const std::string &displayed_id() const
    {
        return displayed_id_;
    }
    // A session has been adopted (true after the first snapshot claim).
    bool adopted() const
    {
        return !displayed_id_.empty();
    }

    const std::string &workdir() const
    {
        return workdir_;
    }
    void set_workdir(std::string workdir)
    {
        workdir_ = std::move(workdir);
    }

    const std::string &chat_log_path() const
    {
        return chat_log_path_;
    }

    // Switch to another session (id + transcript path together). The old workdir
    // is invalidated at once: rather than show a known-wrong value, the caller
    // falls back to the process launch directory until the next snapshot fills it.
    void Reset(std::string id, std::string chat_log_path)
    {
        displayed_id_ = std::move(id);
        chat_log_path_ = std::move(chat_log_path);
        workdir_.clear();
    }

  private:
    std::string displayed_id_;
    std::string workdir_;
    std::string chat_log_path_;
};

} // namespace edc::model

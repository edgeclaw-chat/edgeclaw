#pragma once

// View: the update dialog (UpdateDialog).
//
// A modal that answers "a newer release exists -- install it?" and then reports what happened.
// It is the only place the app writes a user-visible message about updates, so it carries the
// two things a user has to know before saying yes:
//
//   * what the update costs: the database migrates forward on first start of the new build,
//     and an older binary will refuse it afterwards. There is no downgrade;
//   * that the restart is theirs to make. The dialog never restarts the app.
//
// Five states, and every one of them is reachable:
//
//   CONFIRM   a newer release exists; [Update] [Cancel]
//   RUNNING   the installer is running; no buttons, Esc abandons
//   DONE      the new binary is in place; "restart to use it"
//   FAILED    the installer did not succeed, with its output and the manual command
//   UNAVAILABLE  this copy cannot upgrade itself (hand-installed, no installer); manual command
//
// The dialog owns no process: it is handed callbacks that start an upgrade and is told the
// result. That keeps the "how" (fetch, spawn, timeout) out of the view, and lets the host
// substitute a fake in tests.

#include "edc_widget.hpp" /* ui::Widget (Template Method base) */

#include "ftxui/component/component_base.hpp"

#include <functional>
#include <string>

namespace edc::ui
{

class UpdateDialog : public Widget
{
  public:
    enum class State
    {
        CONFIRM = 0,
        RUNNING,
        DONE,
        FAILED,
        UNAVAILABLE,
    };

    /* Start an upgrade for |tag|. The host kicks off the work and later calls OnResult. */
    using StartFn = std::function<void(const std::string &tag)>;
    /* Ask for the manual command to show (the host knows the version and the platform). */
    using HintFn = std::function<std::string()>;
    /* Abandon an upgrade in flight. Without this, Esc would only hide the dialog and the
     * installer would keep running with nobody watching -- the worst of both. */
    using CancelFn = std::function<void()>;

    UpdateDialog(StartFn on_start, HintFn on_hint, CancelFn on_cancel);

    /* Show the confirmation for |tag|, or the "cannot upgrade here" page when |can_upgrade| is
     * false (a copy the installer did not put there, or a platform without one). */
    void Open(const std::string &tag, bool can_upgrade);
    void Close();
    bool IsOpen() const
    {
        return open_;
    }

    State state() const
    {
        return state_;
    }

    /* The host reports the outcome. Ignored unless we are RUNNING, so a late callback from an
     * abandoned upgrade cannot overwrite a newer dialog. */
    void OnResult(bool ok, const std::string &detail);

    /* Esc: available in every state. While RUNNING it means "abandon": the host is told to
     * cancel, which kills the installer if it is still running. It cannot undo an install that
     * already completed, which is why the confirmation warns before the user starts. */
    bool OnEvent(::ftxui::Event event) override;

  protected:
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    ::ftxui::Element ButtonsElement() const;
    void Begin(); /* CONFIRM -> RUNNING and hand the work to the host */

    StartFn on_start_;
    HintFn on_hint_;
    CancelFn on_cancel_;

    bool open_ = false;
    State state_ = State::CONFIRM;
    std::string tag_;
    std::string detail_;
    int button_ = 0; /* 0 = the primary action, 1 = cancel/close */
};

} // namespace edc::ui

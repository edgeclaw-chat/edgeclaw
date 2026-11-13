#pragma once

// View: the update banner's state machine, and what /update does in each state.
//
// Kept out of TUIWindow so it can be tested. The install runs in the background and the dialog
// closes as soon as it is confirmed, which makes this banner the only thing that reports
// progress -- four states, and each one asks the user for something different (nothing, wait,
// restart, retry). Asserting that on a rendered frame is the only way to know the wording did
// not silently drift into promising an action that is not available.
//
// A pure module: the state is an argument, not a member, and nothing here reads the app.

#include "ftxui/dom/elements.hpp"

#include <string>

namespace edc::ui
{

enum class UpdateBanner
{
    None,      /* nothing found, or the check has not landed yet: no banner */
    Available, /* a newer release exists */
    Running,   /* the installer is working, in the background */
    Done,      /* the binary is replaced; only a restart helps */
    Failed,    /* it did not finish; it can be retried */
};

/* What `/update` means in a given state. */
enum class UpdateCommand
{
    NoUpdate,       /* nothing to offer */
    OpenDialog,     /* ask for consent (first time, or a retry after a failure) */
    AlreadyRunning, /* say so: there is nothing to retry and nothing to cancel here */
    RestartNeeded,  /* say so: the work is done, the timing is the user's */
};

UpdateCommand UpdateCommandFor(UpdateBanner state);

/* The one-line hint that goes in the paste-hint slot for a |UpdateCommand|. Empty for
 * OpenDialog, which opens a dialog instead of saying anything. */
const char *UpdateCommandHint(UpdateCommand command);

/* The banner itself, one line, borderless (the interface draws no boxes). |tag| is the release
 * the state is about; |detail| is the installer's own output when it failed, and is ignored in
 * every other state. */
::ftxui::Element UpdateBannerElement(UpdateBanner state, const std::string &tag, const std::string &detail);

} // namespace edc::ui

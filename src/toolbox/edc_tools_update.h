#ifndef __BC_EDC_TOOLS_UPDATE_H__
#define __BC_EDC_TOOLS_UPDATE_H__

#include "edc_tools.h"

#include <stdbool.h>

/* `update_install`: replace this binary with a published release.
 *
 * Registered as INTERNAL, never as a normal tool. It is the one tool that must never be
 * reachable by the model: replacing the running program is a user decision, not something a
 * conversation may decide. The kernel seeds the call itself
 * (`edc_task_spec.initial_tool_name`) when the user confirms an update in the dialog, so the
 * work still gets a task, a sidebar line and a history entry -- the model just never sees the
 * tool that does it.
 *
 * Why a tool at all, rather than calling the upgrade directly: the download-and-install is a
 * long, fallible operation, and the task ledger already knows how to carry one (pending call
 * -> result -> settle, with the task visible the whole time). The `!cmd` path established
 * that an ordinary task driven by a seeded call is how "do this without asking a model" is
 * spelled here.
 *
 * Layering: the toolbox sits below the app and cannot include edc_app.h, so the upgrade is
 * injected. |start| mirrors edc_app_update_start and returns false when nothing can start
 * (already running, this copy cannot self-upgrade). |on_done| fires on the aoop thread with
 * the outcome; the tool converts it into the call's result. */

/* Called to begin the upgrade for |tag|.
 *
 * Two pointers on purpose, and they are NOT interchangeable: |start_ud| is whatever was
 * registered (the app, for edc_app_update_start), while |done_ud| belongs to the caller of
 * this function and must come back to |on_done| unchanged. An earlier version passed one
 * pointer for both, which meant the completion callback received the app where it expected
 * its own state -- a type confusion that would only show up at the end of a successful
 * upgrade.
 *
 * |on_done| is invoked exactly once, with ok and a human-readable detail (the installer's own
 * output when it failed). */
typedef bool (*edc_update_install_fn)(void *start_ud, const char *tag,
                                      void (*on_done)(bool ok, const char *detail, void *ud), void *done_ud);

/* Register the tool on |tb| as internal. |start| / |ud| are stored, not owned: they must
 * outlive the toolbox (the app owns both). */
extern void edc_tools_update_register(struct edc_toolbox *tb, edc_update_install_fn start, void *ud);

#endif /* __BC_EDC_TOOLS_UPDATE_H__ */

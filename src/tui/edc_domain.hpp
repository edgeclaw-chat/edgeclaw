#pragma once

// Domain rules: snapshot claiming + live-output routing (policy objects).
//
// These were free functions; they are now policy objects with the same default
// behaviour (the old free functions remain as thin delegates), so a caller can
// inject a different policy in tests.

#include "edc_task_snapshot.hpp"

#include <string>

namespace edc::domain
{

// Snapshot claiming: whether the frontend accepts a tree, and whether it pulls one.
class SnapshotClaimPolicy
{
  public:
    virtual ~SnapshotClaimPolicy() = default;

    // Is the received tree the one currently displayed? (Late replies are dropped;
    // an empty displayed_session lets the bootstrap frame claim the slot.)
    virtual bool Claims(const TaskSnapshot &snap, const std::string &displayed_session) const = 0;

    // "session |event_session| changed for |reason|" -> pull?
    virtual bool WantsPull(const std::string &event_session, const std::string &reason,
                           const std::string &displayed_session) const = 0;

    // Stateless default implementation; process-wide shared.
    static const SnapshotClaimPolicy &Default();
};

// Where one live output goes.
enum class OutputRoute
{
    Shown,        // the current view + current transcript (active task of the shown session)
    Silent,       // do nothing (a non-active task of the same session)
    OtherSession, // into its own session's transcript (visible when switched back)
};

// Live-output routing policy.
class OutputRoutingPolicy
{
  public:
    virtual ~OutputRoutingPolicy() = default;

    // Output with no session tag (command replies) is always Shown.
    virtual OutputRoute Route(const std::string &session_id, bool from_active_task,
                              const std::string &displayed_session) const = 0;

    static const OutputRoutingPolicy &Default();
};

} // namespace edc::domain

#include "edc_domain.hpp"

namespace edc::domain
{

namespace
{

// Default claim policy: the semantics of the old free functions, verbatim.
class PullOnDisplayedPolicy : public SnapshotClaimPolicy
{
  public:
    bool Claims(const TaskSnapshot &snap, const std::string &displayed_session) const override
    {
        if (snap.session_id.empty())
            return false;
        /* Nothing adopted yet: let the first reply claim the slot. Snapshots are
         * only ever replies to a pull, so the one that can arrive here is the
         * answer to the bootstrap pull -- it names the active session. */
        if (displayed_session.empty())
            return true;
        return displayed_session == snap.session_id;
    }

    bool WantsPull(const std::string &event_session, const std::string &reason,
                   const std::string &displayed_session) const override
    {
        if (event_session.empty())
            return false;
        // Tab switch: the caller already adopted the new session, so pull.
        if (reason == "active")
            return true;
        // Otherwise only the displayed tree pulls.
        return event_session == displayed_session;
    }
};

// Default routing: the displayed session only shows its active task's output.
class ForegroundOnlyRouting : public OutputRoutingPolicy
{
  public:
    OutputRoute Route(const std::string &session_id, bool from_active_task,
                      const std::string &displayed_session) const override
    {
        if (session_id.empty())
            return OutputRoute::Shown; // not a task's output (command reply)
        if (session_id != displayed_session)
            return OutputRoute::OtherSession;
        return from_active_task ? OutputRoute::Shown : OutputRoute::Silent;
    }
};

} // namespace

const SnapshotClaimPolicy &SnapshotClaimPolicy::Default()
{
    static const PullOnDisplayedPolicy kInstance;
    return kInstance;
}

const OutputRoutingPolicy &OutputRoutingPolicy::Default()
{
    static const ForegroundOnlyRouting kInstance;
    return kInstance;
}

} // namespace edc::domain

#include "edc_domain.hpp"

namespace edc::domain
{

namespace
{

// 默认认领策略: 就是原来两个自由函数的语义, 逐字搬过来, 不引入新判断。
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
        // 切台事件: 调用方已先把显示换过去, 必须拉。
        if (reason == "active")
            return true;
        // 其余: 只有显示中的那棵才拉。
        return event_session == displayed_session;
    }
};

// 默认路由策略: 显示中的 session 只显示它 active task 的产出。
class ForegroundOnlyRouting : public OutputRoutingPolicy
{
  public:
    OutputRoute Route(const std::string &session_id, bool from_active_task,
                      const std::string &displayed_session) const override
    {
        if (session_id.empty())
            return OutputRoute::Shown; // 不是某个 task 的产出 (命令回复等)
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

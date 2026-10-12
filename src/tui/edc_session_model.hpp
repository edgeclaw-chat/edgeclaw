#pragma once

#include <string>
#include <utility>

namespace edc::model
{

// 前端"正在显示哪个会话"的状态。
//
// 从前这三个值 (显示中的 session id / 它绑定的工作目录 / 它的转录路径) 是
// TUIWindow 的裸成员, 散在提交、页脚、切换、转录几处读写。现在收进一个模型,
// 让"显示的身份"有一个单一归属, 也让分派器可以只读它来判断归属。
class SessionModel
{
  public:
    const std::string &displayed_id() const
    {
        return displayed_id_;
    }
    // 已经采纳过某个会话 (第一帧快照自证身份之后为真)。
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

    // 切到另一个会话 (id + 转录路径一起换)。旧的工作目录立即作废 —— 与其显示一个
    // 已知不对的值, 不如让调用方回退到进程启动目录, 等下一帧快照回填。
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

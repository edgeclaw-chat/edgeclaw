#pragma once

// ── 端口: 会话转录存储 (TranscriptStore) ─────────────────────────────────────
//
// 转录是 TUI 自己"上过屏的东西"的记录 (每屏一条气泡一行), 从前这段 I/O 写在
// TUIWindow 里: 解析路径、确保父目录、修复半截行、symlink-safe 追加、重放。
// 现在收进一个存储对象:
//
//   * TUIWindow (及其 replay 回调) 只管"把条目变成气泡上屏", 不再碰文件格式;
//   * 文件格式 (EncodeTranscriptLine / LoadTranscript / TranscriptEndsMidLine)
//     集中在这一层, 单独可测;
//   * 路径经 IAppFacade 解析 (它知道 workspace), 存储不认识 app 的具体类型。

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

    // 按会话 id 解析转录路径 (`<workspace>/<session_id>/chat.jsonl`)。
    virtual bool PathFor(const std::string &session_id, std::string *out) const = 0;

    // 只读重放: 每条可解码的行交 |emit|, 返回条数 (0 = 没有可用转录)。
    virtual size_t Replay(const std::string &path,
                          const std::function<void(const ChatTranscriptEntry &)> &emit) const = 0;

    // 追加一条 (先确保父目录存在、修复可能存在的半截行、symlink-safe)。
    virtual void Append(const std::string &path, const ChatTranscriptEntry &entry) = 0;
};

// IAppFacade 的生产实现。
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

#pragma once

// ── 端口层: 把"外部世界"(时间 / 终端 / 主题)挡在视图之外 ───────────────────
//
// 现在这些依赖是从环境里各自现取的: TickAnimations 直接读 steady_clock,
// RenderFooter 直接 ioctl(TIOCGWINSZ), 每个 widget 直接调全局 style::palette()。
// 隐藏依赖让组件无法脱离真实终端单测。
//
// 这里把它们提成**接口**: 组件依赖接口, 生产路径注入真实现, 测试注入假实现
// (假时钟驱动一帧不必 sleep; 固定宽度验证 footer 的裁剪阈值; 假调色板断言组件
// 只用了调色板角色)。

#include "edc_style.hpp"

#include <chrono>
#include <memory>

namespace edc::port
{

// 时间源。动画与节流读它。
class IClock
{
  public:
    using TimePoint = std::chrono::steady_clock::time_point;

    virtual ~IClock() = default;
    virtual TimePoint now() const = 0;
};

// 生产实现: 系统单调时钟。
class SystemClock : public IClock
{
  public:
    TimePoint now() const override
    {
        return std::chrono::steady_clock::now();
    }

    // 进程级共享实例 (无状态, 单例安全)。
    static std::shared_ptr<const SystemClock> Shared();
};

// 终端几何。footer 按可用宽度裁剪字段。
class ITerminalInfo
{
  public:
    virtual ~ITerminalInfo() = default;
    // 终端列数 (无法取得时返回一个稳妥的默认 80)。
    virtual int width() const = 0;
};

class TerminalInfo : public ITerminalInfo
{
  public:
    int width() const override;

    static std::shared_ptr<const TerminalInfo> Shared();
};

// 调色板端口: 组件依赖它, 而不是全局 style::palette()。
class IPalette
{
  public:
    virtual ~IPalette() = default;
    virtual const style::Palette &colors() const = 0;
};

// 生产实现: 转发到启动时解析好的全局调色板 (见 edc_style.hpp / edc_theme.hpp)。
class ThemePalette : public IPalette
{
  public:
    const style::Palette &colors() const override
    {
        return style::palette();
    }

    static std::shared_ptr<const ThemePalette> Shared();
};

} // namespace edc::port

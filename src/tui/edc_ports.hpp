#pragma once

// Port layer: keeps the "outside world" (time / terminal) out of the views.
//
// These dependencies used to be taken from the environment in place (TickAnimations
// read steady_clock, RenderFooter did ioctl(TIOCGWINSZ)), which made components
// untestable outside a real terminal. Raised to interfaces: components depend on
// the interface, production injects the real one, tests inject a fake (a fake
// clock drives a frame without sleeping; a fixed width checks the footer's trim
// threshold).
//
// The palette port (style::IPalette / style::SetPaletteProvider) lives with
// Palette in edc_style.hpp -- it returns that type, so keeping them together
// avoids a cycle.

#include "edc_style.hpp"

#include <chrono>
#include <memory>

namespace edc::port
{

// Time source, read by animations and throttles.
class IClock
{
  public:
    using TimePoint = std::chrono::steady_clock::time_point;

    virtual ~IClock() = default;
    virtual TimePoint now() const = 0;
};

// Production: the system monotonic clock.
class SystemClock : public IClock
{
  public:
    TimePoint now() const override
    {
        return std::chrono::steady_clock::now();
    }

    // Process-wide shared instance (stateless, singleton-safe).
    static std::shared_ptr<const SystemClock> Shared();
};

// Terminal geometry; the footer trims its fields against this width.
class ITerminalInfo
{
  public:
    virtual ~ITerminalInfo() = default;
    // Terminal columns (a safe default of 80 when unavailable).
    virtual int width() const = 0;
};

class TerminalInfo : public ITerminalInfo
{
  public:
    int width() const override;

    static std::shared_ptr<const TerminalInfo> Shared();
};

} // namespace edc::port

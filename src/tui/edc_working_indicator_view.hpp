#pragma once

// Animated row: WorkingIndicatorView.
//
// The row that replaces the footer while a foreground task runs: a block bar that
// sweeps back and forth, with the brightness inside the lit run flowing along the
// direction of travel. It used to be a free function WorkingIndicatorElement()
// plus a pile of constants in an anonymous namespace; now it is a component that
// holds an injected clock, counts from Restart(), and draws the current duration
// in Draw(). The old free function remains as a thin delegate.
//
// It replaces the footer, so its height is always one row and its cell count is
// fixed (independent of width), and the layout does not jump when it swaps in.

#include "edc_ports.hpp"
#include "edc_widget.hpp"

#include <ftxui/dom/elements.hpp>

#include <memory>

namespace edc::ui
{

// The animated "working" row. |clock| is the injected time source; nullptr falls
// back to SystemClock::Shared().
class WorkingIndicatorView : public Widget
{
  public:
    explicit WorkingIndicatorView(std::shared_ptr<const port::IClock> clock = nullptr);

    // Start counting from "now" (called when the foreground process turns busy).
    void Restart();

    // Stop counting (called when the process is no longer busy).
    void Stop();

    // True while counting from the last Restart().
    bool Running() const
    {
        return running_;
    }

    // Milliseconds since the last Restart(); 0 when not running.
    long long ElapsedMs() const;

    // Pure render of the frame at |elapsed_ms|: stateless, so tests and stateless
    // callers can draw a given moment without an instance.
    static ::ftxui::Element ElementAt(long long elapsed_ms);

  protected:
    ::ftxui::Element Draw(const Frame &frame) override;

  private:
    std::shared_ptr<const port::IClock> clock_;
    port::IClock::TimePoint started_{};
    bool running_ = false;
};

} // namespace edc::ui

namespace edc
{
// Old entry point (kept): equivalent to WorkingIndicatorView::ElementAt.
::ftxui::Element WorkingIndicatorElement(long long elapsed_ms);
} // namespace edc

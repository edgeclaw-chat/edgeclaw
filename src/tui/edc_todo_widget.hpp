#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/box.hpp"
#include <string>
#include <vector>

namespace edc
{
// Right-sidebar widget below the task tree: renders the latest todo-list
// status driven by the update_todolist tool. The tool emits a structured JSON
// payload (its raw args, {"items":[{content,status,activeForm},...]}) next to
// the Markdown checklist it echoes into the chat log; the TUI window parses
// that payload (C side, edc_c_todo_payload_parse) and hands the widget plain
// items here. Each emission carries the full list — this widget replaces its
// state wholesale, there is no incremental update.
class TodoWidget : public ::ftxui::ComponentBase
{
  public:
    struct Item
    {
        std::string content;
        std::string status; /* pending | in_progress | completed */
        std::string active_form;
    };

    TodoWidget();

    // Replace the whole displayed list. A call with no items clears the
    // widget (the agent reported an empty list).
    void SetItems(const std::vector<Item> &items);

    // True while a non-empty list is displayed; the enclosing pane only
    // reserves space for the widget while it has something to show.
    bool HasItems() const
    {
        return !items_.empty();
    }

    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event event) override
    {
        return false;
    }
    bool Focusable() const override
    {
        return false;
    }

  private:
    std::vector<Item> items_;
    int completed_ = 0;

    // Cap the rows drawn so a huge list cannot swallow the task tree above;
    // leftovers are summarized with a "+N more" line.
    static constexpr size_t kMaxVisibleRows = 10;

    // Content box of this widget's inner vbox (rows), written by reflect()
    // during the previous render pass. OnRender reads it to truncate item
    // contents to the real row width. All-zero means "not measured yet".
    ::ftxui::Box content_box_;
};
} // namespace edc

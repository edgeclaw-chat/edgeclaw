#include "edc_connect_dialog.hpp"

#include "edc_style.hpp"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <algorithm>
#include <utility>

namespace edc::ui
{

ConnectDialog::ConnectDialog(Submit submit) : submit_(std::move(submit))
{
    // Fields do NOT use ftxui::Input's default transform: it paints the focused
    // field with `inverted`, i.e. a full-width slab in the terminal's default
    // background (white on the dark palette). The terminal draws the text caret
    // in that same colour, so the caret vanished inside the slab -- the field
    // looked like it had no cursor at all. Draw the value in the palette's own
    // foreground and dim the placeholder; focus is shown by the accent label in
    // Draw() (and by the caret itself), matching the app's flat style.
    ::ftxui::InputOption field_option;
    field_option.transform = [](const ::ftxui::InputState &state) {
        const auto &pal = style::palette();
        ::ftxui::Element element = state.element | ::ftxui::color(pal.text);
        if (state.is_placeholder)
            element |= ::ftxui::dim;
        return element;
    };
    // Enter while a field is focused is still the primary action (commit the
    // form). It is wired here, per field, so that the dialog does not have to
    // swallow Return globally -- doing that also stole it from Connect/Cancel,
    // making a keyboard-selected Cancel save the form (see OnEvent).
    field_option.on_enter = [this] { SubmitNow(); };
    // Single-line fields: with ftxui's default (multiline = true) Return would
    // both insert a '\n' and submit, so a failed submit (empty field) left a
    // newline behind in the field.
    field_option.multiline = false;

    // Bind each field's caret position so Open() can move it to the end of the
    // pre-filled value. Without this the caret starts at 0 and typing prepends,
    // which reads as "the caret is at the wrong end".
    field_option.cursor_position = &url_cursor_;
    url_input_ = ::ftxui::Input(&url_, "https://api.example.com/v1", field_option);
    field_option.cursor_position = &model_cursor_;
    model_input_ = ::ftxui::Input(&model_, "model name", field_option);
    field_option.cursor_position = &key_cursor_;
    key_input_ = ::ftxui::Input(&key_, "API key", field_option);

    // Flat, borderless buttons. ButtonOption::Simple()/Border() draw a boxed frame
    // around the label, which reads as heavy next to the flat dialog; focus is shown
    // as the accent highlight instead, matching the active Tasks/Todos rows.
    ::ftxui::ButtonOption flat_button;
    flat_button.transform = [](const ::ftxui::EntryState &s) {
        const auto &pal = style::palette();
        auto label = ::ftxui::text(" " + s.label + " ");
        if (s.focused)
            return label | ::ftxui::bold | ::ftxui::color(pal.accent);
        return label | ::ftxui::color(pal.text);
    };

    auto ok = ::ftxui::Button("Connect", [this] { SubmitNow(); }, flat_button);
    auto cancel = ::ftxui::Button("Cancel", [this] { Close(); }, flat_button);
    buttons_ = ::ftxui::Container::Horizontal({ok, cancel}, &button_focus_);
    fields_ = ::ftxui::Container::Vertical({url_input_, model_input_, key_input_, buttons_}, &focus_);

    // Adopt the form as a real component child. Focusable()/ActiveChild() walk
    // the component tree, so without this the dialog looks child-less: its
    // Focusable() is false, Focused() reports false, and the sibling tie-break in
    // the window's `dbox({backdrop, dialog})` hands the caret to the backdrop's
    // chat input -- the dialog would draw with no cursor at all. Draw() still
    // renders the fields itself (Widget::OnRender is final); the child link only
    // exists to carry focus.
    Add(fields_);
}

void ConnectDialog::Open(const port::ConnectConfig &initial)
{
    url_ = initial.api_url;
    model_ = initial.model;
    key_ = initial.api_key;
    // Park the caret at the end of each pre-filled value: the user is editing an
    // existing value, and the next keystroke should append.
    url_cursor_ = static_cast<int>(url_.size());
    model_cursor_ = static_cast<int>(model_.size());
    key_cursor_ = static_cast<int>(key_.size());
    focus_ = 0;
    button_focus_ = 0;
    open_ = true;
}

void ConnectDialog::MoveRow(int dir)
{
    focus_ = std::max(0, std::min(kRows - 1, focus_ + dir));
}

void ConnectDialog::Close()
{
    open_ = false;
}

bool ConnectDialog::SubmitNow()
{
    if (url_.empty() || model_.empty() || key_.empty())
        return false;
    port::ConnectConfig cfg;
    cfg.api_url = url_;
    cfg.model = model_;
    cfg.api_key = key_;
    if (!submit_ || !submit_(cfg))
        return false;
    Close();
    return true;
}

bool ConnectDialog::OnEvent(::ftxui::Event event)
{
    if (!open_)
        return false;
    // Esc closes without saving; everything else goes to the fields. Return is
    // deliberately NOT handled here: it must reach whatever is focused, so a
    // focused field submits through its own on_enter (see the constructor) while
    // the focused Connect/Cancel button runs only its own action. Intercepting
    // Return here made Cancel submit-and-save too, because the button never saw
    // the key.
    if (event == ::ftxui::Event::Escape)
    {
        Close();
        return true;
    }
    // Up/Down walk the rows here, before the focused field can see them. The
    // fields are single-line, but ftxui's Input still consumes Up/Down for its
    // own (line) caret: a first Down only jumped the caret to the end of the
    // text and a second Down finally changed the row. One press, one row.
    if (event == ::ftxui::Event::ArrowDown || event == ::ftxui::Event::ArrowUp)
    {
        MoveRow(event == ::ftxui::Event::ArrowDown ? +1 : -1);
        return true;
    }
    return fields_->OnEvent(event);
}

::ftxui::Element ConnectDialog::Draw(const Frame &)
{
    // Flat, borderless dialog borrowing the sidebar's visual language (accent
    // heading, dim rules, flat labels) but floating on dialog_bg. Labels use the
    // bright foreground, not text_dim, which washes out on that surface.
    // BorderStyle::EMPTY reserves the inset without drawing box lines.
    using namespace ::ftxui;
    const auto &pal = style::palette();

    // Focused field = accent label (same signal the buttons and sidebar rows
    // use). It must not be a background fill: the terminal caret sits on the
    // value and would disappear into it (see the constructor).
    auto field = [&](const char *label, const Component &input) {
        auto label_element = input->Focused() ? text(label) | color(pal.accent) | bold : text(label) | color(pal.text);
        return hbox({
            std::move(label_element),
            input->Render() | flex,
        });
    };

    return vbox({
               text(" Connect provider ") | bold | color(pal.accent),
               separator() | color(pal.line_dim),
               field("API URL  ", url_input_),
               field("Model    ", model_input_),
               field("API Key  ", key_input_),
               separator() | color(pal.line_dim),
               hbox({
                   text(" ") | dim | color(pal.text_dim),
                   filler(),
                   buttons_->Render(),
               }),
           }) |
           borderStyled(BorderStyle::EMPTY) | bgcolor(pal.dialog_bg);
}

} // namespace edc::ui

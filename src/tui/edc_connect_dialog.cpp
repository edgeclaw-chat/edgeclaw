#include "edc_connect_dialog.hpp"

#include "edc_style.hpp"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <utility>

namespace edc::ui
{

ConnectDialog::ConnectDialog(Submit submit) : submit_(std::move(submit))
{
    url_input_ = ::ftxui::Input(&url_, "https://api.example.com/v1/chat/completions");
    model_input_ = ::ftxui::Input(&model_, "model name");
    key_input_ = ::ftxui::Input(&key_, "API key");

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
}

void ConnectDialog::Open(const port::ConnectConfig &initial)
{
    url_ = initial.api_url;
    model_ = initial.model;
    key_ = initial.api_key;
    focus_ = 0;
    button_focus_ = 0;
    open_ = true;
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
    // Esc closes it, Enter submits, everything else goes to its fields.
    if (event == ::ftxui::Event::Escape)
    {
        Close();
        return true;
    }
    if (event == ::ftxui::Event::Return)
    {
        SubmitNow();
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

    auto field = [&](const char *label, const Component &input) {
        return hbox({
            text(label) | color(pal.text),
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

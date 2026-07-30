#include "edc_chat_widget.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

namespace edc
{

ChatWidget::ChatWidget()
{
    container_ = ::ftxui::Container::Vertical({});
}

void ChatWidget::AddMessage(std::shared_ptr<Message> message)
{
    container_->Add(message);
    container_->Add(::ftxui::Renderer([] { return ::ftxui::separatorEmpty(); }));
}

bool ChatWidget::OnEvent(::ftxui::Event event)
{
    return container_->OnEvent(event);
}

::ftxui::Element ChatWidget::OnRender()
{
    return container_->Render() | ::ftxui::bgcolor(::ftxui::Color::RGB(10, 10, 10));
}

} // namespace edc

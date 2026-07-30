#pragma once
#include <ftxui/dom/elements.hpp>
#include <string_view>

namespace edc
{

ftxui::Element RenderMarkdown(std::string_view md);

} // namespace edc

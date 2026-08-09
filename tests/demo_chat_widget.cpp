#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "tui/edc_chat_widget.hpp"
#include <ctime>
#include <memory>

using namespace ftxui;

int main()
{
    auto now = time(nullptr);

    auto chat = std::make_shared<edc::ChatWidget>();

    // ---- User message (plain paragraph) ----
    chat->AddMessage(std::make_shared<edc::UserMessage>("你好，帮我看看这几个 markdown 特性渲染得对不对。", now));

#if 1
    // ---- Base Message with headings ----
    chat->AddMessage(std::make_shared<edc::Message>("# 一级标题\n"
                                                    "这是一段正文。\n"
                                                    "## 二级标题\n"
                                                    "这是另一段正文。\n"
                                                    "### 三级标题\n"
                                                    "第三段正文。",
                                                    now));

    // ---- Bold / Italic / Strikethrough / Link ----
    chat->AddMessage(std::make_shared<edc::Message>("以下是行内格式测试：\n\n"
                                                    "**粗体文字** 和 *斜体文字* 和 ~~删除线~~。\n"
                                                    "还有一个 [链接](https://example.com) 在这里。\n"
                                                    "以及 `code` 行内代码。",
                                                    now));

    // ---- Fenced code block ----
    chat->AddMessage(std::make_shared<edc::Message>("下面是一个 C++ 代码块：\n\n"
                                                    "```cpp\n"
                                                    "#include <iostream>\n"
                                                    "int main() {\n"
                                                    "    std::cout << \"Hello, world!\" << std::endl;\n"
                                                    "    return 0;\n"
                                                    "}\n"
                                                    "```",
                                                    now));

    // ---- Unordered list ----
    chat->AddMessage(std::make_shared<edc::Message>("无序列表：\n\n"
                                                    "- 第一项\n"
                                                    "- 第二项\n"
                                                    "  - 嵌套项 A\n"
                                                    "  - 嵌套项 B\n"
                                                    "- 第三项",
                                                    now));

    // ---- Ordered list ----
    chat->AddMessage(std::make_shared<edc::Message>("有序列表：\n\n"
                                                    "1. 第一步\n"
                                                    "2. 第二步\n"
                                                    "3. 第三步",
                                                    now));

    // ---- Blockquote ----
    chat->AddMessage(std::make_shared<edc::Message>("引用块：\n\n"
                                                    "> 这是一段引用文字。\n"
                                                    "> 可以有多行。\n"
                                                    ">\n"
                                                    "> 引用里面还可以有 **粗体**。",
                                                    now));
#endif

#if 1
    // ---- Horizontal rule ----
    chat->AddMessage(std::make_shared<edc::Message>(
        "上面是一条分割线：\n\n"
        "---\n\n"
        "分割线下面的文字。",
        now));

    // ---- Table ----
    chat->AddMessage(std::make_shared<edc::Message>(
        "表格：\n\n"
        "| 列 A | 列 B | 列 C |\n"
        "|------|------|------|\n"
        "| a1   | b1   | c1   |\n"
        "| a2   | b2   | c2   |",
        now));

    // ---- Mixed complex content ----
    chat->AddMessage(std::make_shared<edc::Message>(
        "## 综合测试\n"
        "这是一段包含多种元素的段落：**粗体**、*斜体*、`code`、\n"
        "[链接](https://example.com) 和 ~~删除线~~。\n\n"
        "> 引用：这是最重要的部分。\n\n"
        "有序：\n"
        "1. 你好\n"
        "2. 世界\n\n"
        "```\n"
        "$ echo hello\n"
        "hello\n"
        "```",
        now));
#endif

    auto root = Renderer(chat, [&] { return chat->Render() | flex; });

    root |= CatchEvent([&](Event e) -> bool {
        if (e == Event::Escape || e == Event::Character('q') || e == Event::CtrlC)
        {
            return true;
        }
        return false;
    });

    auto screen = ScreenInteractive::Fullscreen();
    screen.Loop(root);

    return 0;
}

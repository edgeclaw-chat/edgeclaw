#include "tui/edc_chat_widget.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/screen/screen.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("[%3d] %-55s ", tests_run, name); } while (0)
#define PASS()   do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while (0)
#define ASSERT(cond, fmt, ...) if (!(cond)) { FAIL(fmt, ##__VA_ARGS__); return; } else do {} while (0)

static void test_render_user(void)
{
    TEST("render user message");
    edc::Message msg("Hello, world!", edc::Message::Role::User, time(nullptr));
    auto element = msg.OnRender();

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);

    std::string output = screen.ToString();
    ASSERT(output.find("you") != std::string::npos, "label 'you' not found");
    ASSERT(output.find("Hello, world!") != std::string::npos, "content not found");

    PASS();
}

static void test_render_assistant(void)
{
    TEST("render assistant message");
    edc::Message msg("I am an assistant.", edc::Message::Role::Assistant, time(nullptr));
    auto element = msg.OnRender();

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);

    std::string output = screen.ToString();
    ASSERT(output.find("assistant") != std::string::npos, "label 'assistant' not found");
    ASSERT(output.find("I am an assistant.") != std::string::npos, "content not found");

    PASS();
}

static void test_render_tool(void)
{
    TEST("render tool message");
    edc::Message msg("tool output here", edc::Message::Role::Tool, time(nullptr));
    auto element = msg.OnRender();

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);

    std::string output = screen.ToString();
    ASSERT(output.find("tool") != std::string::npos, "label 'tool' not found");
    ASSERT(output.find("tool output here") != std::string::npos, "content not found");

    PASS();
}

static void test_render_system(void)
{
    TEST("render system message");
    edc::Message msg("system message", edc::Message::Role::System, time(nullptr));
    auto element = msg.OnRender();

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);

    std::string output = screen.ToString();
    ASSERT(output.find("system") != std::string::npos, "label 'system' not found");
    ASSERT(output.find("system message") != std::string::npos, "content not found");

    PASS();
}

static void test_render_user_color(void)
{
    TEST("user message has yellow label");
    edc::Message msg("color test", edc::Message::Role::User, time(nullptr));
    auto element = msg.OnRender();

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);

    const auto &cell = screen.CellAt(0, 0);
    ASSERT(cell.foreground_color == ftxui::Color::Yellow, "user label not yellow");

    PASS();
}

static void test_render_assistant_color(void)
{
    TEST("assistant message has green label");
    edc::Message msg("color test", edc::Message::Role::Assistant, time(nullptr));
    auto element = msg.OnRender();

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);

    const auto &cell = screen.CellAt(0, 0);
    ASSERT(cell.foreground_color == ftxui::Color::Green, "assistant label not green");

    PASS();
}

static void test_render_tool_color(void)
{
    TEST("tool message has blue label");
    edc::Message msg("color test", edc::Message::Role::Tool, time(nullptr));
    auto element = msg.OnRender();

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);

    const auto &cell = screen.CellAt(0, 0);
    ASSERT(cell.foreground_color == ftxui::Color::Blue, "tool label not blue");

    PASS();
}

static void test_render_label_bold(void)
{
    TEST("label is bold");
    edc::Message msg("bold test", edc::Message::Role::Assistant, time(nullptr));
    auto element = msg.OnRender();

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);

    const auto &cell = screen.CellAt(0, 0);
    ASSERT(cell.bold, "label not bold");

    PASS();
}

static void test_on_event_noop(void)
{
    TEST("OnEvent returns false (no events handled)");
    edc::Message msg("event test", edc::Message::Role::User, time(nullptr));

    ASSERT(!msg.OnEvent(ftxui::Event::Character('a')), "OnEvent should return false");
    PASS();
}

int main()
{
    printf("=== Message Component Render Tests ===\n\n");

    test_render_user();
    test_render_assistant();
    test_render_tool();
    test_render_system();
    test_render_user_color();
    test_render_assistant_color();
    test_render_tool_color();
    test_render_label_bold();
    test_on_event_noop();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_run == tests_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

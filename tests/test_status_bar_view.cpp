// StatusBarView 的右段丢弃策略单测 (纯函数, 无需终端/屏幕)。
//
// 页脚右端只留四样: "model · ctx 45k/128k · cache 78% · sandbox:on|off"; 窄屏时从最前面往外丢
// 字段: 先丢 model, 再丢 ctx, 再丢 cache, 永远保留 sandbox。这里**不出现** shell:on|off 与 /help:
// 开关状态不是常驻信息（它的效果在输入行上体现），/help 挪进了输入框的 placeholder。
// 这条策略原来藏在 TUIWindow::RenderFooter() 里, 只能靠渲染整个窗口验证; 现在它是
// StatusBarView::RightText() 纯函数, 直接钉住。
//
// 另外钉住 ContextText(): 上下文容量那一格的格式 ("ctx 45k/128k" / 估算加 "~" /
// 无窗口时只给已用值)。

#include "edc_status_bar_view.hpp"
#include "ftxui/screen/string.hpp"

#include <iostream>
#include <string>

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    if (ok)
    {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    g_failures++;
}

const char *kLeft = " /home/user/project";
const char *kModel = "gpt-x";
const char *kCtx = "ctx 45k/128k";
const int kLeftW = static_cast<int>(std::string(kLeft).size());

void TestWideShowsEverything()
{
    std::cout << "wide pane: model + the sandbox state, nothing else\n";
    // No context yet: the field is simply absent. Both sandbox states have to render.
    const std::string r =
        edc::ui::StatusBarView::RightText(80, kLeft, kModel, /*ctx=*/"", /*cache=*/"", /*sandbox_on=*/true);
    Check(r == std::string(kModel) + " · sandbox:on", "all fields present (got: " + r + ")");
    Check(edc::ui::StatusBarView::RightText(80, kLeft, kModel, "", /*cache=*/"", /*sandbox_on=*/false) ==
              std::string(kModel) + " · sandbox:off",
          "the sandbox field reflects its flag");
    Check(r.find("shell:") == std::string::npos, "no shell switch cell (it is not a footer field)");
    Check(r.find("/help") == std::string::npos, "/help is not a footer field (it moved to the input)");
}

void TestWideWithContext()
{
    std::cout << "wide pane: the context field sits between model and the sandbox state\n";
    const std::string r = edc::ui::StatusBarView::RightText(80, kLeft, kModel, kCtx, /*cache=*/"", true);
    Check(r == std::string(kModel) + " · " + kCtx + " · sandbox:on", "ctx shown (got: " + r + ")");
}

void TestNarrowDropsModelFirst()
{
    std::cout << "narrow pane: drop model, keep the sandbox state\n";
    const int pane = kLeftW + 1 + ::ftxui::string_width(std::string("sandbox:on"));
    const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, "", /*cache=*/"", true);
    Check(r == "sandbox:on", "model dropped, the sandbox state kept (got: " + r + ")");
}

void TestContextDroppedBeforeSandbox()
{
    std::cout << "narrower pane: drop model then ctx, keep the sandbox state\n";
    // One column short of "ctx 45k/128k · sandbox:on": the model goes first, then ctx.
    // (string_width, not size(): the "·" is two bytes but one cell.)
    const std::string cand = std::string(kCtx) + " · sandbox:on";
    const int pane = kLeftW + 1 + ::ftxui::string_width(cand) - 1;
    const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, kCtx, /*cache=*/"", true);
    Check(r == "sandbox:on", "model + ctx dropped (got: " + r + ")");
}

void TestNarrowestKeepsTheSandboxState()
{
    std::cout << "narrowest pane: the sandbox state is the last thing to go (it never does)\n";
    // Room for "sandbox:on" alone; one column less and the field is still returned
    // (the loop floors at the last field rather than dropping everything).
    for (int extra = 0; extra >= -4; extra--)
    {
        const int pane = kLeftW + 1 + ::ftxui::string_width(std::string("sandbox:on")) + extra;
        const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, kCtx, /*cache=*/"", true);
        Check(r == "sandbox:on", "still just the sandbox state (pane " + std::to_string(pane) + ", got: " + r + ")");
    }
}

void TestEmptyModelNeverShowsAModelField()
{
    std::cout << "no model name: the sandbox state only\n";
    const std::string r = edc::ui::StatusBarView::RightText(80, kLeft, /*model=*/"", "", /*cache=*/"", true);
    Check(r == "sandbox:on", "empty model -> no leading field (got: " + r + ")");
}

void TestContextText()
{
    std::cout << "the context field reads as used/window\n";
    Check(edc::ui::StatusBarView::ContextText(45000, false, 128000) == "ctx 45k/128k", "used + window");
    Check(edc::ui::StatusBarView::ContextText(45000, true, 128000) == "ctx ~45k/128k",
          "an estimate wears a tilde");
    Check(edc::ui::StatusBarView::ContextText(45000, false, 0) == "ctx 45k", "no window -> used only");
    Check(edc::ui::StatusBarView::ContextText(0, false, 128000) == "", "nothing used -> hidden");
}

void TestCacheText()
{
    std::cout << "the cache field reads as a hit rate over the conversation's input\n";
    Check(edc::ui::StatusBarView::CacheText(78, 100) == "cache 78%", "half-ish hit rate");
    Check(edc::ui::StatusBarView::CacheText(0, 1000) == "cache 0%", "a measured miss is a real number");
    // No usage yet must NOT read as a miss: an absent field and a measured 0% are
    // different claims, and the footer must not make the worse one by accident.
    Check(edc::ui::StatusBarView::CacheText(0, 0) == "", "nothing reported -> hidden, not 0%");
    Check(edc::ui::StatusBarView::CacheText(150, 100) == "cache 100%", "clamped to 100");
}

void TestCacheFieldSitsBeforeSandbox()
{
    std::cout << "wide pane: the cache rate sits between ctx and the sandbox state\n";
    const std::string cache = edc::ui::StatusBarView::CacheText(78, 100);
    const std::string r = edc::ui::StatusBarView::RightText(80, kLeft, kModel, kCtx, cache, true);
    Check(r == std::string(kModel) + " · " + kCtx + " · " + cache + " · sandbox:on", "cache shown (got: " + r + ")");
}

void TestLeftText()
{
    std::cout << "the left end is the dir, or the hint in its place\n";
    Check(edc::ui::StatusBarView::LeftText("/home/x", "") == " /home/x", "no hint -> just the dir");
    Check(edc::ui::StatusBarView::LeftText("/home/x", "Ctrl+C again to quit") ==
              " Ctrl+C again to quit",
          "the hint replaces the dir (same corner)");
}

} // namespace

int main()
{
    TestWideShowsEverything();
    TestWideWithContext();
    TestNarrowDropsModelFirst();
    TestContextDroppedBeforeSandbox();
    TestNarrowestKeepsTheSandboxState();
    TestEmptyModelNeverShowsAModelField();
    TestContextText();
    TestCacheText();
    TestCacheFieldSitsBeforeSandbox();
    TestLeftText();

    if (g_failures == 0)
    {
        std::cout << "\nall passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " FAILED\n";
    return 1;
}

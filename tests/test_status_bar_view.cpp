// StatusBarView 的右段丢弃策略单测 (纯函数, 无需终端/屏幕)。
//
// 页脚右端是 "model · sandbox:on|off · /help"; 窄屏时从中间往外丢字段: 先丢 model,
// 再丢 sandbox, 永远保留 "/help"。这条策略原来藏在 TUIWindow::RenderFooter() 里,
// 只能靠渲染整个窗口验证; 现在它是 StatusBarView::RightText() 纯函数, 直接钉住。

#include "edc_status_bar_view.hpp"

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
const int kLeftW = static_cast<int>(std::string(kLeft).size());

void TestWideShowsEverything()
{
    std::cout << "wide pane: model + sandbox + /help\n";
    // Plenty of room: all three fields fit on one line.
    const std::string r = edc::ui::StatusBarView::RightText(80, kLeft, kModel, /*sandbox_on=*/true);
    Check(r == std::string(kModel) + " · sandbox:on · /help", "all fields present (got: " + r + ")");
    Check(edc::ui::StatusBarView::RightText(80, kLeft, kModel, false) ==
              std::string(kModel) + " · sandbox:off · /help",
          "sandbox state reflects the flag");
}

void TestNarrowDropsModelFirst()
{
    std::cout << "narrow pane: drop model, keep sandbox + /help\n";
    // Room for "sandbox:on · /help" but not for the model: the model goes first.
    const int pane = kLeftW + 1 + static_cast<int>(std::string("sandbox:on · /help").size());
    const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, true);
    Check(r == "sandbox:on · /help", "model dropped, sandbox kept (got: " + r + ")");
}

void TestNarrowerDropsSandboxToo()
{
    std::cout << "narrower pane: only /help survives\n";
    // Room for "/help" alone.
    const int pane = kLeftW + 1 + static_cast<int>(std::string("/help").size());
    const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, true);
    Check(r == "/help", "both model and sandbox dropped, /help survives (got: " + r + ")");
}

void TestEmptyModelNeverShowsAModelField()
{
    std::cout << "no model name: sandbox + /help only\n";
    const std::string r = edc::ui::StatusBarView::RightText(80, kLeft, /*model=*/"", true);
    Check(r == "sandbox:on · /help", "empty model -> no leading field (got: " + r + ")");
}

} // namespace

int main()
{
    TestWideShowsEverything();
    TestNarrowDropsModelFirst();
    TestNarrowerDropsSandboxToo();
    TestEmptyModelNeverShowsAModelField();

    if (g_failures == 0)
    {
        std::cout << "\nall passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " FAILED\n";
    return 1;
}

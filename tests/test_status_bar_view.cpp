// StatusBarView 的右段丢弃策略单测 (纯函数, 无需终端/屏幕)。
//
// 页脚右端是 "model · ctx 45k/128k · sandbox:on|off · shell:on|off · /help"; 窄屏时从
// 最前面往外丢字段: 先丢 model, 再丢 ctx, 再丢 sandbox, 再丢 shell, 永远保留 "/help"。
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
    std::cout << "wide pane: model + both switches + /help\n";
    // No context yet: the field is simply absent. The shell switch is the default (off)
    // in the first case, on in the second -- both states have to render.
    const std::string r =
        edc::ui::StatusBarView::RightText(80, kLeft, kModel, /*ctx=*/"", /*sandbox_on=*/true, /*shell_on=*/false);
    Check(r == std::string(kModel) + " · sandbox:on · shell:off · /help", "all fields present (got: " + r + ")");
    Check(edc::ui::StatusBarView::RightText(80, kLeft, kModel, "", false, true) ==
              std::string(kModel) + " · sandbox:off · shell:on · /help",
          "each switch reflects its own flag");
}

void TestWideWithContext()
{
    std::cout << "wide pane: the context field sits between model and the switches\n";
    const std::string r = edc::ui::StatusBarView::RightText(80, kLeft, kModel, kCtx, true, false);
    Check(r == std::string(kModel) + " · " + kCtx + " · sandbox:on · shell:off · /help",
          "ctx shown (got: " + r + ")");
}

void TestNarrowDropsModelFirst()
{
    std::cout << "narrow pane: drop model, keep the switches + /help\n";
    const int pane = kLeftW + 1 + ::ftxui::string_width(std::string("sandbox:on · shell:off · /help"));
    const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, "", true, false);
    Check(r == "sandbox:on · shell:off · /help", "model dropped, both switches kept (got: " + r + ")");
}

void TestContextDroppedBeforeSandbox()
{
    std::cout << "narrower pane: drop model then ctx, keep the switches + /help\n";
    // One column short of "ctx 45k/128k · sandbox:on · shell:off · /help": the model goes
    // first, then ctx, leaving the switches + /help. (string_width, not size(): the "·"
    // is two bytes but one cell.)
    const std::string cand = std::string(kCtx) + " · sandbox:on · shell:off · /help";
    const int pane = kLeftW + 1 + ::ftxui::string_width(cand) - 1;
    const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, kCtx, true, false);
    Check(r == "sandbox:on · shell:off · /help", "model + ctx dropped (got: " + r + ")");
}

void TestSandboxDroppedBeforeShell()
{
    std::cout << "narrower still: the sandbox switch goes before the shell one\n";
    // Room for "shell:off · /help" but not for "sandbox:on · shell:off · /help": the drop
    // order is model, ctx, sandbox, shell -- sandbox is the older, always-on-default
    // switch, so it is the one that yields first.
    const int pane = kLeftW + 1 + ::ftxui::string_width(std::string("shell:off · /help"));
    const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, kCtx, true, false);
    Check(r == "shell:off · /help", "sandbox dropped, shell kept (got: " + r + ")");
}

void TestNarrowerDropsBothSwitches()
{
    std::cout << "narrowest pane: only /help survives\n";
    // Room for "/help" alone.
    const int pane = kLeftW + 1 + static_cast<int>(std::string("/help").size());
    const std::string r = edc::ui::StatusBarView::RightText(pane, kLeft, kModel, kCtx, true, false);
    Check(r == "/help", "everything but /help dropped (got: " + r + ")");
}

void TestEmptyModelNeverShowsAModelField()
{
    std::cout << "no model name: the switches + /help only\n";
    const std::string r = edc::ui::StatusBarView::RightText(80, kLeft, /*model=*/"", "", true, false);
    Check(r == "sandbox:on · shell:off · /help", "empty model -> no leading field (got: " + r + ")");
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
    TestSandboxDroppedBeforeShell();
    TestNarrowerDropsBothSwitches();
    TestEmptyModelNeverShowsAModelField();
    TestContextText();
    TestLeftText();

    if (g_failures == 0)
    {
        std::cout << "\nall passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " FAILED\n";
    return 1;
}

// Regression test for the /connect dialog's text cursor.
//
// The dialog is composed on top of the live window through a `dbox`: the
// backdrop keeps its own focused input (the chat draft) and the dialog is the
// top-most child. FTXUI picks the cursor from the focused requirement that
// wins the sibling tie-break, so the dialog only owns the cursor if its own
// component is reported as Focused(). Because ConnectDialog holds its fields as
// plain members instead of component children, Focusable() used to fall back to
// "has no focusable child" -> false, the backdrop's focused input won the
// tie-break, and the dialog showed no caret at all. This renders exactly that
// composition and asserts the caret lands on the dialog's first field.

#include "edc_connect_dialog.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp> // Render
#include <ftxui/screen/screen.hpp>

#include <cstdio>
#include <memory>
#include <string>

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            std::printf("    ");                                                                                       \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static std::string row_text(const ftxui::Screen &screen, int y, int width)
{
    std::string out;
    for (int x = 0; x < width; ++x)
        out += screen.CellAt(x, y).character;
    return out;
}

int main()
{
    using namespace ftxui;

    // The backdrop keeps a focused input of its own, exactly like the chat
    // draft underneath the modal in the real window.
    std::string backdrop_text;
    auto backdrop = Input(&backdrop_text, "type a message");

    // Empty config -> all three fields render their placeholder; the caret then
    // has a well-defined home on the first field's row.
    edc::port::ConnectConfig initial;
    edc::ui::ConnectDialog dialog([](const edc::port::ConnectConfig &) { return true; });
    dialog.Open(initial);
    CHECK(dialog.IsOpen(), "dialog should be open after Open()");

    const int width = 100;
    const int height = 30;

    auto screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
    auto doc = dbox({
        backdrop->Render() | dim,
        dialog.Render() | clear_under | size(WIDTH, LESS_THAN, 80) | center,
    });
    Render(screen, doc);

    // Find the dialog's "API URL" row (its first field).
    int url_row = -1;
    for (int y = 0; y < height; ++y)
    {
        if (row_text(screen, y, width).find("API URL") != std::string::npos)
        {
            url_row = y;
            break;
        }
    }
    CHECK(url_row >= 0, "the dialog's API URL field should be on screen");

    const auto cursor = screen.cursor();
    std::printf("  cursor at (%d,%d), shape=%d; API URL row=%d\n", cursor.x, cursor.y, (int)cursor.shape, url_row);

    CHECK(cursor.shape != Screen::Cursor::Hidden, "the dialog must set a visible caret shape");
    CHECK(cursor.y == url_row, "the caret must sit on the dialog's first field, not on the backdrop");

    // The caret can only be seen if the cell it sits on is not reverse-video: a
    // background slab there is exactly what hid it (an `inverted` field paints
    // white on the dark palette, same colour as the terminal caret).
    CHECK(!screen.PixelAt(cursor.x, cursor.y).inverted,
          "the focused field must not be drawn inverted or the caret disappears into it");

    // --- Buttons ------------------------------------------------------------------
    // A filled config, so SubmitNow() would succeed if it were (wrongly) reached.
    edc::port::ConnectConfig filled;
    filled.api_url = "https://api.example.com/v1/chat/completions";
    filled.model = "test-model";
    filled.api_key = "sk-test";

    auto open_with = [&](int *submits) {
        auto d = std::make_shared<edc::ui::ConnectDialog>([submits](const edc::port::ConnectConfig &) {
            (*submits)++;
            return true;
        });
        d->Open(filled);
        return d;
    };

    // Tab walks the row selector (ArrowUp/Down never reach it: the focused
    // ftxui::Input consumes them for its own cursor).
    auto select_cancel = [](edc::ui::ConnectDialog &d) {
        d.OnEvent(Event::Tab); // URL -> Model
        d.OnEvent(Event::Tab); // Model -> API Key
        d.OnEvent(Event::Tab); // API Key -> buttons (Connect focused)
        d.OnEvent(Event::Tab); // Connect -> Cancel
    };
    auto select_connect = [](edc::ui::ConnectDialog &d) {
        d.OnEvent(Event::Tab); // URL -> Model
        d.OnEvent(Event::Tab); // Model -> API Key
        d.OnEvent(Event::Tab); // API Key -> buttons (Connect is first)
    };

    // Enter on Cancel must NOT save: a keyboard-selected Cancel used to submit
    // anyway, because OnEvent swallowed every Return before the buttons saw it.
    {
        int submits = 0;
        auto d = open_with(&submits);
        select_cancel(*d);
        d->OnEvent(Event::Return);
        CHECK(submits == 0, "Enter on Cancel must not save the form (submits=%d)", submits);
        CHECK(!d->IsOpen(), "Enter on Cancel must close the dialog");
    }

    // Enter on Connect still saves and closes.
    {
        int submits = 0;
        auto d = open_with(&submits);
        select_connect(*d);
        d->OnEvent(Event::Return);
        CHECK(submits == 1, "Enter on Connect must submit (submits=%d)", submits);
        CHECK(!d->IsOpen(), "a successful submit closes the dialog");
    }

    // Enter while a text field is focused is the primary action: it still saves.
    {
        int submits = 0;
        auto d = open_with(&submits);
        d->OnEvent(Event::Return); // first field is focused
        CHECK(submits == 1, "Enter in a field must submit (submits=%d)", submits);
        CHECK(!d->IsOpen(), "a successful submit closes the dialog");
    }

    // Esc always closes without saving.
    {
        int submits = 0;
        auto d = open_with(&submits);
        d->OnEvent(Event::Escape);
        CHECK(submits == 0, "Esc must not save (submits=%d)", submits);
        CHECK(!d->IsOpen(), "Esc must close the dialog");
    }

    // --- Row navigation and caret placement ---------------------------------------
    // The caret starts at the end of the pre-filled value (typing appends), and a
    // single Down/Up changes the row -- it used to be eaten by the field's own
    // line-caret, so the first Down only jumped the caret to the end.
    auto submit_probe = [&](int *submits, edc::port::ConnectConfig *out) {
        return std::make_shared<edc::ui::ConnectDialog>([submits, out](const edc::port::ConnectConfig &c) {
            (*submits)++;
            *out = c;
            return true;
        });
    };

    // Caret at the end on open: the first keystroke appends to the URL.
    {
        int submits = 0;
        edc::port::ConnectConfig out;
        auto d = submit_probe(&submits, &out);
        d->Open(filled);
        d->OnEvent(Event::Character('X'));
        d->OnEvent(Event::Return);
        CHECK(out.api_url == filled.api_url + "X", "caret must start at the end (got '%s')", out.api_url.c_str());
    }

    // One Down = one row: typing lands in Model, not in the URL.
    {
        int submits = 0;
        edc::port::ConnectConfig out;
        auto d = submit_probe(&submits, &out);
        d->Open(filled);
        d->OnEvent(Event::ArrowDown); // URL -> Model, in one press
        d->OnEvent(Event::Character('X'));
        d->OnEvent(Event::Return);
        CHECK(out.api_url == filled.api_url, "Down must leave the URL untouched (got '%s')", out.api_url.c_str());
        CHECK(out.model == filled.model + "X", "Down then typing must append to Model (got '%s')", out.model.c_str());
    }

    // Up returns to the previous row.
    {
        int submits = 0;
        edc::port::ConnectConfig out;
        auto d = submit_probe(&submits, &out);
        d->Open(filled);
        d->OnEvent(Event::ArrowDown);
        d->OnEvent(Event::ArrowUp);
        d->OnEvent(Event::Character('X'));
        d->OnEvent(Event::Return);
        CHECK(out.api_url == filled.api_url + "X", "Up must return to the URL (got '%s')", out.api_url.c_str());
        CHECK(out.model == filled.model, "Up must leave Model untouched (got '%s')", out.model.c_str());
    }

    // Up from the button row comes back to the last field.
    {
        int submits = 0;
        edc::port::ConnectConfig out;
        auto d = submit_probe(&submits, &out);
        d->Open(filled);
        d->OnEvent(Event::ArrowDown); // Model
        d->OnEvent(Event::ArrowDown); // API Key
        d->OnEvent(Event::ArrowDown); // buttons
        d->OnEvent(Event::ArrowUp);   // back to API Key
        d->OnEvent(Event::Character('X'));
        d->OnEvent(Event::Return);
        CHECK(out.api_key == filled.api_key + "X", "Up from the buttons must reach API Key (got '%s')", out.api_key.c_str());
    }

    if (g_failures == 0)
        std::printf("test_connect_dialog: OK\n");
    return g_failures == 0 ? 0 : 1;
}

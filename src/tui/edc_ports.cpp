#include "edc_ports.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace edc::port
{

std::shared_ptr<const SystemClock> SystemClock::Shared()
{
    static const std::shared_ptr<const SystemClock> kInstance = std::make_shared<SystemClock>();
    return kInstance;
}

int TerminalInfo::width() const
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    struct winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
#endif
}

std::shared_ptr<const TerminalInfo> TerminalInfo::Shared()
{
    static const std::shared_ptr<const TerminalInfo> kInstance = std::make_shared<TerminalInfo>();
    return kInstance;
}

} // namespace edc::port

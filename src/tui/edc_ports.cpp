#include "edc_ports.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

namespace edc::port
{

std::shared_ptr<const SystemClock> SystemClock::Shared()
{
    static const std::shared_ptr<const SystemClock> kInstance = std::make_shared<SystemClock>();
    return kInstance;
}

int TerminalInfo::width() const
{
    struct winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

std::shared_ptr<const TerminalInfo> TerminalInfo::Shared()
{
    static const std::shared_ptr<const TerminalInfo> kInstance = std::make_shared<TerminalInfo>();
    return kInstance;
}

} // namespace edc::port

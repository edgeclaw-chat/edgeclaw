/* edgeclaw_cli: the TUI's input box, without the screen.
 *
 * A separate binary on purpose. It links the app and NOT ftxui, so a container image or a
 * cron host ships a frontend that cannot take over a terminal even by accident -- and it is
 * the smaller artefact by a wide margin. The interactive `edgeclaw` binary contains no
 * headless code at all.
 *
 * Everything it does with its arguments lives in edc_cli.c; this file is only the process
 * boundary (argv, exit code). */

#include "edc_cli.h"
#include "edc_log.h"

int main(int argc, char **argv)
{
    edc_log_init();
    const int code = edc_cli_main(argc, argv);
    edc_log_close();
    return code;
}

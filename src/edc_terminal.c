#include "edc_terminal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
int edc_terminal_save(int fd, struct edc_terminal_state *saved)
{
    HANDLE h = (fd == STDIN_FILENO) ? GetStdHandle(STD_INPUT_HANDLE) : NULL;
    if (!h || !GetConsoleMode(h, &saved->original_mode))
        return -1;
    saved->valid = true;
    return 0;
}

void edc_terminal_restore(int fd, struct edc_terminal_state *saved)
{
    if (!saved || !saved->valid) return;
    HANDLE h = (fd == STDIN_FILENO) ? GetStdHandle(STD_INPUT_HANDLE) : NULL;
    if (!h) return;
    SetConsoleMode(h, saved->original_mode);
    saved->valid = false;
}

int edc_terminal_echo_off(int fd, struct edc_terminal_state *saved)
{
    HANDLE h;
    if (fd == STDIN_FILENO)
        h = GetStdHandle(STD_INPUT_HANDLE);
    else
        return -1;

    if (!GetConsoleMode(h, &saved->original_mode))
        return -1;
    saved->valid = true;

    DWORD mode = saved->original_mode & ~ENABLE_ECHO_INPUT;
    if (!SetConsoleMode(h, mode)) {
        saved->valid = false;
        return -1;
    }
    return 0;
}

void edc_terminal_echo_restore(int fd, struct edc_terminal_state *saved)
{
    if (!saved || !saved->valid) return;
    HANDLE h = (fd == STDIN_FILENO) ? GetStdHandle(STD_INPUT_HANDLE) : NULL;
    if (!h) return;
    SetConsoleMode(h, saved->original_mode);
    saved->valid = false;
}

int edc_terminal_raw_mode(int fd, struct edc_terminal_state *saved)
{
    return edc_terminal_echo_off(fd, saved);
}

void edc_terminal_raw_restore(int fd, struct edc_terminal_state *saved)
{
    edc_terminal_echo_restore(fd, saved);
}
#else /* POSIX */
int edc_terminal_save(int fd, struct edc_terminal_state *saved)
{
    if (!isatty(fd)) return -1;
    if (tcgetattr(fd, &saved->original) != 0) return -1;
    saved->valid = true;
    return 0;
}

void edc_terminal_restore(int fd, struct edc_terminal_state *saved)
{
    if (!saved || !saved->valid) return;
    tcsetattr(fd, TCSANOW, &saved->original);
    saved->valid = false;
}

int edc_terminal_echo_off(int fd, struct edc_terminal_state *saved)
{
    if (!isatty(fd)) return -1;
    if (tcgetattr(fd, &saved->original) != 0) return -1;
    saved->valid = true;
    struct termios t = saved->original;
    t.c_lflag &= ~(ECHO | ECHONL);
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        saved->valid = false;
        return -1;
    }
    return 0;
}

void edc_terminal_echo_restore(int fd, struct edc_terminal_state *saved)
{
    if (!saved || !saved->valid) return;
    tcsetattr(fd, TCSANOW, &saved->original);
    saved->valid = false;
}

int edc_terminal_raw_mode(int fd, struct edc_terminal_state *saved)
{
    if (!isatty(fd)) return -1;
    if (tcgetattr(fd, &saved->original) != 0) return -1;
    saved->valid = true;
    struct termios t = saved->original;
    cfmakeraw(&t);
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        saved->valid = false;
        return -1;
    }
    return 0;
}

void edc_terminal_raw_restore(int fd, struct edc_terminal_state *saved)
{
    edc_terminal_echo_restore(fd, saved);
}
#endif

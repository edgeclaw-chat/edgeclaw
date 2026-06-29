#ifndef EDC_TERMINAL_H
#define EDC_TERMINAL_H

#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

struct edc_terminal_state
{
#ifdef _WIN32
    DWORD original_mode;
    bool valid;
#else
    struct termios original;
    bool valid;
#endif
};

int edc_terminal_save(int fd, struct edc_terminal_state *saved);
void edc_terminal_restore(int fd, struct edc_terminal_state *saved);

int edc_terminal_echo_off(int fd, struct edc_terminal_state *saved);
void edc_terminal_echo_restore(int fd, struct edc_terminal_state *saved);

int edc_terminal_raw_mode(int fd, struct edc_terminal_state *saved);
void edc_terminal_raw_restore(int fd, struct edc_terminal_state *saved);

#endif

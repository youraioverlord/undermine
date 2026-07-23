/* Native "open file" dialog. Kept in its own translation unit so <windows.h>
 * never shares a compile with raylib.h (they collide on many names). */
#ifndef FILEDLG_H
#define FILEDLG_H

#include <stdbool.h>

/* Show a modal file picker for .mole bots. On OK, writes the chosen path into
 * `out` (NUL-terminated, truncated to outlen) and returns true; returns false
 * if the user cancels or the platform has no dialog. */
bool filedlg_open_mole(char *out, int outlen);

#endif

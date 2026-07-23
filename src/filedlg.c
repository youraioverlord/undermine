/* filedlg — native open-file dialog (see filedlg.h). Windows uses the common
 * dialog (comdlg32); other platforms currently have no picker. */
#define _CRT_SECURE_NO_WARNINGS   /* strncpy is used deliberately here */
#include "filedlg.h"
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

bool filedlg_open_mole(char *out, int outlen)
{
    OPENFILENAMEA ofn;
    char file[1024];
    file[0] = '\0';
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = GetActiveWindow();   /* the game window, so the dialog is modal to it */
    ofn.lpstrFilter = "MoleScript bots (*.mole)\0*.mole\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof file;
    ofn.lpstrTitle  = "Load a .mole bot";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return false;   /* cancelled or error */
    if (outlen > 0) { strncpy(out, file, (size_t)outlen - 1); out[outlen - 1] = '\0'; }
    return true;
}

#else

bool filedlg_open_mole(char *out, int outlen)
{
    (void)out; (void)outlen;
    return false;
}

#endif

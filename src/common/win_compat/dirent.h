#ifndef EDC_WIN_COMPAT_DIRENT_H
#define EDC_WIN_COMPAT_DIRENT_H

/*
 * <dirent.h> for MSVC, backed by FindFirstFileW/FindNextFileW.
 *
 * The directory APIs are a genuine gap in the CRT, not a naming difference, so this one has
 * to implement opendir/readdir/closedir rather than alias them. The wide-character Win32 API
 * is used on purpose: paths in this project are UTF-8, and the ANSI variants would mangle any
 * non-ASCII name under a non-UTF-8 code page. d_name is converted back to UTF-8 for the same
 * reason.
 *
 * Each translation unit gets its own static copies. That is fine (they hold no global state)
 * and it avoids adding a Windows-only .c file and a library target just for three functions.
 *
 * d_type is filled in because the callers rely on it: edc_file_utils.c switches on
 * DT_DIR / DT_REG / DT_LNK. Filesystem entries that are symlinks or reparse points report
 * DT_LNK and are otherwise reported as DT_REG/DT_DIR; anything the Win32 data does not
 * determine is DT_UNKNOWN, which callers already handle (they fall back to stat()).
 */

#ifdef _MSC_VER

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

struct dirent
{
    unsigned char d_type;
    /* A UTF-16 name can expand up to 3 bytes per unit in UTF-8, plus the terminator. */
    char d_name[MAX_PATH * 3 + 1];
};

typedef struct edc_win_dir
{
    HANDLE handle;
    WIN32_FIND_DATAW data;
    int done;
    struct dirent entry;
} DIR;

static __inline wchar_t *edc_win_dir_utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0)
        return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0)
    {
        free(w);
        return NULL;
    }
    return w;
}

static __inline void edc_win_dir_wide_to_utf8(const wchar_t *w, char *out, size_t cap)
{
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) <= 0)
        out[0] = '\0';
}

static __inline DIR *opendir(const char *path)
{
    wchar_t *wide;
    wchar_t *pattern;
    size_t len;
    DIR *dir;

    if (path == NULL)
        return NULL;
    wide = edc_win_dir_utf8_to_wide(path);
    if (wide == NULL)
        return NULL;

    len = wcslen(wide);
    /* "C:\dir" -> "C:\dir\*"; an already-trailing separator must not become "\\*". */
    pattern = (wchar_t *)malloc((len + 3) * sizeof(wchar_t));
    if (pattern == NULL)
    {
        free(wide);
        return NULL;
    }
    wcscpy(pattern, wide);
    if (len > 0 && pattern[len - 1] != L'\\' && pattern[len - 1] != L'/')
        wcscat(pattern, L"\\");
    wcscat(pattern, L"*");
    free(wide);

    dir = (DIR *)calloc(1, sizeof(DIR));
    if (dir == NULL)
    {
        free(pattern);
        return NULL;
    }
    dir->handle = FindFirstFileW(pattern, &dir->data);
    free(pattern);
    if (dir->handle == INVALID_HANDLE_VALUE)
    {
        free(dir);
        return NULL;
    }
    return dir;
}

static __inline struct dirent *readdir(DIR *dir)
{
    if (dir == NULL || dir->done)
        return NULL;

    edc_win_dir_wide_to_utf8(dir->data.cFileName, dir->entry.d_name, sizeof(dir->entry.d_name));

    if ((dir->data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        dir->entry.d_type = DT_LNK;
    else if ((dir->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        dir->entry.d_type = DT_DIR;
    else
        dir->entry.d_type = DT_REG;

    /* Advance for the next call; when the enumeration ends, the entry just filled in is the
     * last one and the following call returns NULL. */
    if (FindNextFileW(dir->handle, &dir->data) == 0)
        dir->done = 1;

    return &dir->entry;
}

static __inline int closedir(DIR *dir)
{
    if (dir == NULL)
        return -1;
    if (dir->handle != INVALID_HANDLE_VALUE)
        FindClose(dir->handle);
    free(dir);
    return 0;
}

#endif /* _MSC_VER */

#endif /* EDC_WIN_COMPAT_DIRENT_H */

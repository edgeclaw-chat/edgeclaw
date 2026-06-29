#ifndef EDC_COMPAT_H
#define EDC_COMPAT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/*
 * Transitional compatibility header for migrating away from GLib.
 * Provides GLib type aliases and memory allocation macros using
 * standard C and POSIX equivalents.
 *
 * This file is temporary scaffolding. Once all files are migrated,
 * these macros should be replaced with direct standard C usage.
 */

#define TRUE  true
#define FALSE false

typedef char  gchar;
typedef int   gint;
typedef bool  gboolean;
typedef int64_t gint64;
typedef int32_t gint32;
typedef unsigned int  guint;
typedef unsigned char guchar;
typedef unsigned long gulong;
typedef void* gpointer;
typedef const void* gconstpointer;
typedef size_t gsize;
typedef ssize_t gssize;

#define g_new(type, n)     ((type*)malloc((n) * sizeof(type)))
#define g_new0(type, n)    ((type*)calloc((n), sizeof(type)))
#define g_malloc0(n)       calloc(1, (n))
#define g_free             free

#define EDC_NEW(type, n)   ((type*)malloc((n) * sizeof(type)))
#define EDC_NEW0(type, n)  ((type*)calloc((n), sizeof(type)))
#define EDC_MALLOC0(n)     calloc(1, (n))

#endif /* EDC_COMPAT_H */

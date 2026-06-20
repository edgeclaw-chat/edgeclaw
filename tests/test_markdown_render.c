#include "src/edc_markdown_render.h"
#include <glib.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <markdown-file>\n", argv[0]);
        return 1;
    }

    GError *error = NULL;
    gchar *contents = NULL;
    if (!g_file_get_contents(argv[1], &contents, NULL, &error)) {
        fprintf(stderr, "read file failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    GString *rendered = edc_markdown_render(contents);
    g_free(contents);

    if (rendered && rendered->str) {
        printf("%s\n", rendered->str);
    }

    g_string_free(rendered, TRUE);
    return 0;
}

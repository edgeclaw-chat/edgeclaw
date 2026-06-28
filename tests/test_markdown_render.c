#include "src/edc_markdown_render.h"
#include <glib.h>
#include "edc_string.h"
#include <stdio.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <markdown-file>\n", argv[0]);
        return 1;
    }

    GError *gerror = NULL;
    char *contents = NULL;
    if (!g_file_get_contents(argv[1], &contents, NULL, &gerror)) {
        fprintf(stderr, "read file failed: %s\n", gerror->message);
        g_error_free(gerror);
        return 1;
    }

    sds rendered = edc_markdown_render(contents);
    free(contents);

    if (rendered && rendered) {
        printf("%s\n", rendered);
    }

    edc_str_free(rendered);
    return 0;
}

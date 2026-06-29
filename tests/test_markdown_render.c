#include "src/edc_markdown_render.h"
#include "src/edc_file_utils.h"
#include "edc_string.h"
#include <stdio.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <markdown-file>\n", argv[0]);
        return 1;
    }

    EdcError *gerror = NULL;
    sds contents = edc_file_read(argv[1], &gerror);
    if (!contents) {
        fprintf(stderr, "read file failed: %s\n", gerror ? gerror->message : "unknown");
        edc_error_free(gerror);
        return 1;
    }

    sds rendered = edc_markdown_render(contents);
    sdsfree(contents);

    if (rendered && rendered) {
        printf("%s\n", rendered);
    }

    edc_str_free(rendered);
    return 0;
}

#ifndef EDC_BUNDLED_COLORS_H
#define EDC_BUNDLED_COLORS_H

#include "edc_bundled_files.h"

/* The built-in vim colorscheme(s) shipped inside the binary, embedded at build
 * time by //:colors_embedded_c. Released into $EDC_HOME/colors on startup by
 * edc_init_env_colors(); like the bundled manual, a file whose bytes differ
 * from the bundle is rewritten so the theme tracks the binary. A fresh
 * config.json selects tokyonight-moon as the default theme. */

extern const struct edc_bundled_file edc_bundled_colors_entries[];
extern const size_t edc_bundled_colors_count;

#endif /* EDC_BUNDLED_COLORS_H */

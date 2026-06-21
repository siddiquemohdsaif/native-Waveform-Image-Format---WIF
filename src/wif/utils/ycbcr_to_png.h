#ifndef WIF_UTILS_YCBCR_TO_PNG_H
#define WIF_UTILS_YCBCR_TO_PNG_H

#include <stddef.h>
#include <stdint.h>

int wif_ycbcr_raw_to_png(
    const char *y_path,
    const char *cb_path,
    const char *cr_path,
    uint32_t width,
    uint32_t height,
    const char *output_png_path,
    char *error_message,
    size_t error_message_size
);

#endif

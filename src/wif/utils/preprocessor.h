#ifndef WIF_UTILS_PREPROCESSOR_H
#define WIF_UTILS_PREPROCESSOR_H

#include <stddef.h>
#include <stdint.h>

typedef struct WifYuv444Image {
    uint32_t width;
    uint32_t height;
    uint8_t *y_plane;
    uint8_t *cb_plane;
    uint8_t *cr_plane;
} WifYuv444Image;

int wif_preprocess_image(
    const wchar_t *input_path,
    const wchar_t *output_dir,
    WifYuv444Image *image,
    char *error_message,
    size_t error_message_size
);

void wif_free_yuv444_image(WifYuv444Image *image);

#endif

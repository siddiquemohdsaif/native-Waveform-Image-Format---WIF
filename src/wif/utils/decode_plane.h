#ifndef WIF_UTILS_DECODE_PLANE_H
#define WIF_UTILS_DECODE_PLANE_H

#include <stddef.h>
#include <stdint.h>

typedef struct WifDecodePlaneResult {
    uint32_t width;
    uint32_t height;
    uint32_t block_size;
    uint32_t dtc_blocks;
    uint32_t wave_blocks;
} WifDecodePlaneResult;

int wif_decode_plane(
    const char *route_map_rans_path,
    uint32_t width,
    uint32_t height,
    const char *dtc_plane_path,
    uint32_t dtc_width,
    uint32_t dtc_height,
    const char *wave_plane_path,
    uint32_t wave_width,
    uint32_t wave_height,
    const char *output_dir,
    WifDecodePlaneResult *result,
    char *error_message,
    size_t error_message_size
);

#endif

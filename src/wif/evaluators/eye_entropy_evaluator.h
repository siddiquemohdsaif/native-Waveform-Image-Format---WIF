#ifndef WIF_EVALUATORS_EYE_ENTROPY_EVALUATOR_H
#define WIF_EVALUATORS_EYE_ENTROPY_EVALUATOR_H

#include <stddef.h>
#include <stdint.h>

typedef struct WifEyeEntropyResult {
    uint32_t width;
    uint32_t height;
    uint32_t block_size;
    uint8_t threshold;
    uint32_t total_block_count;
    uint32_t route_map_byte_count;
    uint32_t dtc_block_count;
    uint32_t wave_block_count;
    uint32_t dtc_plane_width;
    uint32_t dtc_plane_height;
    uint32_t wave_plane_width;
    uint32_t wave_plane_height;
} WifEyeEntropyResult;

int wif_evaluate_raw_plane(
    const wchar_t *input_path,
    uint32_t width,
    uint32_t height,
    const wchar_t *output_dir,
    uint8_t threshold,
    WifEyeEntropyResult *result,
    char *error_message,
    size_t error_message_size
);

#endif

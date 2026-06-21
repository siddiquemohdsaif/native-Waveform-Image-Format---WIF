#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

#include "wif/evaluators/eye_entropy_evaluator.h"

static wchar_t *utf8_to_wide(const char *text)
{
    wchar_t *wide;
    int count;

    if (text == NULL) {
        return NULL;
    }

    count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (count <= 0) {
        return NULL;
    }

    wide = (wchar_t *)calloc((size_t)count, sizeof(wchar_t));
    if (wide == NULL) {
        return NULL;
    }

    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }

    return wide;
}

static void print_usage(const char *program_name)
{
    fprintf(stderr, "usage: %s <raw-plane> <width> <height> <output-directory> [threshold]\n", program_name);
}

int main(int argc, char **argv)
{
    wchar_t *input_path = NULL;
    wchar_t *output_dir = NULL;
    WifEyeEntropyResult result;
    char error_message[256] = {0};
    uint32_t width;
    uint32_t height;
    uint8_t threshold = 10;
    int exit_code = 1;

    if (argc != 5 && argc != 6) {
        print_usage(argv[0]);
        return 2;
    }

    width = (uint32_t)strtoul(argv[2], NULL, 10);
    height = (uint32_t)strtoul(argv[3], NULL, 10);
    if (argc == 6) {
        unsigned long parsed_threshold = strtoul(argv[5], NULL, 10);
        if (parsed_threshold > 255) {
            fprintf(stderr, "threshold must be 0..255\n");
            return 2;
        }
        threshold = (uint8_t)parsed_threshold;
    }

    input_path = utf8_to_wide(argv[1]);
    output_dir = utf8_to_wide(argv[4]);
    if (input_path == NULL || output_dir == NULL) {
        fprintf(stderr, "failed to read command line paths\n");
        goto cleanup;
    }

    if (!wif_evaluate_raw_plane(
            input_path,
            width,
            height,
            output_dir,
            threshold,
            &result,
            error_message,
            sizeof(error_message))) {
        fprintf(stderr, "EyeEntropyEvaluator failed: %s\n", error_message[0] ? error_message : "unknown error");
        goto cleanup;
    }

    printf("evaluated %u x %u raw plane\n", result.width, result.height);
    printf("block size: %u x %u\n", result.block_size, result.block_size);
    printf("threshold: %u\n", result.threshold);
    printf("total blocks: %u\n", result.total_block_count);
    printf("dtc blocks: %u\n", result.dtc_block_count);
    printf("dtc plane: %u x %u\n", result.dtc_plane_width, result.dtc_plane_height);
    printf("wave blocks: %u\n", result.wave_block_count);
    printf("wave plane: %u x %u\n", result.wave_plane_width, result.wave_plane_height);
    printf("wrote eye_entropy/edge.png and edge.raw\n");
    printf("wrote eye_entropy/dtc_plane.png and dtc_plane.raw\n");
    printf("wrote eye_entropy/wave_plane.png and wave_plane.raw\n");
    printf("wrote eye_entropy/route_map.bin (%u bytes)\n", result.route_map_byte_count);
    printf("wrote eye_entropy/wave_dtc.png\n");
    exit_code = 0;

cleanup:
    free(input_path);
    free(output_dir);
    return exit_code;
}

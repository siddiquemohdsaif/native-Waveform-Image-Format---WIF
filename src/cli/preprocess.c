#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

#include "wif/utils/preprocessor.h"

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
    fprintf(stderr, "usage: %s <input-image> <output-directory>\n", program_name);
}

int main(int argc, char **argv)
{
    wchar_t *input_path = NULL;
    wchar_t *output_dir = NULL;
    WifYuv444Image image;
    char error_message[256] = {0};
    int result = 1;

    if (argc != 3) {
        print_usage(argv[0]);
        return 2;
    }

    input_path = utf8_to_wide(argv[1]);
    output_dir = utf8_to_wide(argv[2]);
    if (input_path == NULL || output_dir == NULL) {
        fprintf(stderr, "failed to read command line paths\n");
        goto cleanup;
    }

    if (!wif_preprocess_image(input_path, output_dir, &image, error_message, sizeof(error_message))) {
        fprintf(stderr, "preprocess failed: %s\n", error_message[0] ? error_message : "unknown error");
        goto cleanup;
    }

    printf("preprocessed %u x %u image\n", image.width, image.height);
    printf("wrote preprocessed/y_plane.png, preprocessed/cb_plane.png, preprocessed/cr_plane.png\n");
    printf("wrote preprocessed/y_plane_buffer.raw, preprocessed/cb_plane_buffer.raw, preprocessed/cr_plane_buffer.raw\n");
    printf("wrote preprocessed/yuv444_planar.yuv\n");
    wif_free_yuv444_image(&image);
    result = 0;

cleanup:
    free(input_path);
    free(output_dir);
    return result;
}

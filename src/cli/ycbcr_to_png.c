#include <stdio.h>
#include <stdlib.h>

#include "wif/utils/ycbcr_to_png.h"

static void print_usage(const char *program_name)
{
    fprintf(stderr, "usage: %s <y.raw> <cb.raw> <cr.raw> <width> <height> <output.png>\n", program_name);
}

int main(int argc, char **argv)
{
    char error_message[256] = {0};

    if (argc != 7) {
        print_usage(argv[0]);
        return 2;
    }

    if (!wif_ycbcr_raw_to_png(
            argv[1],
            argv[2],
            argv[3],
            (uint32_t)strtoul(argv[4], NULL, 10),
            (uint32_t)strtoul(argv[5], NULL, 10),
            argv[6],
            error_message,
            sizeof(error_message))) {
        fprintf(stderr, "YCbCr to PNG failed: %s\n", error_message[0] ? error_message : "unknown error");
        return 1;
    }

    printf("wrote color PNG: %s\n", argv[6]);
    return 0;
}

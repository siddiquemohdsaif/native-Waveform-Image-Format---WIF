#include <stdio.h>
#include <stdlib.h>

#include "wif/utils/decode_plane.h"

static void print_usage(const char *program_name)
{
    fprintf(stderr, "usage: %s <route-map-v1.4.rans> <width> <height> <dtc-plane> <dtc-width> <dtc-height> <wave-plane> <wave-width> <wave-height> <output-dir>\n", program_name);
}

int main(int argc, char **argv)
{
    WifDecodePlaneResult result;
    char error_message[256] = {0};

    if (argc != 11) {
        print_usage(argv[0]);
        return 2;
    }

    if (!wif_decode_plane(
            argv[1],
            (uint32_t)strtoul(argv[2], NULL, 10),
            (uint32_t)strtoul(argv[3], NULL, 10),
            argv[4],
            (uint32_t)strtoul(argv[5], NULL, 10),
            (uint32_t)strtoul(argv[6], NULL, 10),
            argv[7],
            (uint32_t)strtoul(argv[8], NULL, 10),
            (uint32_t)strtoul(argv[9], NULL, 10),
            argv[10],
            &result,
            error_message,
            sizeof(error_message))) {
        fprintf(stderr, "decodePlane failed: %s\n", error_message[0] ? error_message : "unknown error");
        return 1;
    }

    printf("decoded %u x %u plane\n", result.width, result.height);
    printf("dtc blocks: %u\n", result.dtc_blocks);
    printf("wave blocks: %u\n", result.wave_blocks);
    printf("wrote decoded_y_plane.raw and decoded_y_plane.png\n");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wif/utils/rans_route_map.h"

static void print_usage(const char *program_name)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s encode <route-map.bin> <bit-count> <output.rans>\n", program_name);
    fprintf(stderr, "  %s decode <input.rans> <output-route-map.bin>\n", program_name);
}

int main(int argc, char **argv)
{
    char error_message[256] = {0};

    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "encode") == 0) {
        uint32_t bit_count;

        if (argc != 5) {
            print_usage(argv[0]);
            return 2;
        }

        bit_count = (uint32_t)strtoul(argv[3], NULL, 10);
        if (!wif_rans_route_map_encode_file(argv[2], bit_count, argv[4], error_message, sizeof(error_message))) {
            fprintf(stderr, "rANS route-map encode failed: %s\n", error_message[0] ? error_message : "unknown error");
            return 1;
        }

        printf("wrote compressed route map: %s\n", argv[4]);
        return 0;
    }

    if (strcmp(argv[1], "decode") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return 2;
        }

        if (!wif_rans_route_map_decode_file(argv[2], argv[3], error_message, sizeof(error_message))) {
            fprintf(stderr, "rANS route-map decode failed: %s\n", error_message[0] ? error_message : "unknown error");
            return 1;
        }

        printf("wrote decoded route map: %s\n", argv[3]);
        return 0;
    }

    print_usage(argv[0]);
    return 2;
}

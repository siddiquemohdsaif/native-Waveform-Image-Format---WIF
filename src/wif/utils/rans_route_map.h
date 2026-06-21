#ifndef WIF_UTILS_RANS_ROUTE_MAP_H
#define WIF_UTILS_RANS_ROUTE_MAP_H

#include <stddef.h>
#include <stdint.h>

int wif_rans_route_map_encode_file(
    const char *input_path,
    uint32_t bit_count,
    const char *output_path,
    char *error_message,
    size_t error_message_size
);

int wif_rans_route_map_decode_file(
    const char *input_path,
    const char *output_path,
    char *error_message,
    size_t error_message_size
);

int wif_rans_route_map_decode_to_memory(
    const char *input_path,
    uint8_t **route_map,
    uint32_t *route_bit_count,
    uint32_t *route_byte_count,
    char *error_message,
    size_t error_message_size
);

#endif

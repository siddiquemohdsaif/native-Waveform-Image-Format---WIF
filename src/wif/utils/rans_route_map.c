#include "wif/utils/rans_route_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIF_RANS14_SCALE_BITS 12u
#define WIF_RANS14_SCALE (1u << WIF_RANS14_SCALE_BITS)
#define WIF_RANS14_BYTE_L (1u << 23)
#define WIF_RANS14_SYMBOL_COUNT 3u
#define WIF_RANS14_RUN_SYMBOL 2u
#define WIF_RANS14_RUN_BASE 6u
#define WIF_RANS14_EXTRA_MAX 275u
#define WIF_RANS14_RUN_MAX (WIF_RANS14_RUN_BASE + WIF_RANS14_EXTRA_MAX)

static const uint8_t WIF_RANS14_MAGIC[8] = {'W', 'I', 'F', 'R', 'L', 'E', '1', '4'};

static void set_error(char *buffer, size_t buffer_size, const char *message)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", message);
}

static uint32_t ceil_div8_u32(uint32_t value)
{
    return (value + 7u) / 8u;
}

static uint8_t get_bit_msb_first(const uint8_t *bytes, uint32_t bit_index)
{
    return (uint8_t)((bytes[bit_index / 8u] >> (7u - (bit_index % 8u))) & 1u);
}

static void set_bit_msb_first(uint8_t *bytes, uint32_t bit_index, uint8_t value)
{
    if (value) {
        bytes[bit_index / 8u] |= (uint8_t)(1u << (7u - (bit_index % 8u)));
    }
}

static void write_u16_le(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset + 0] = (uint8_t)(value & 0xffu);
    bytes[offset + 1] = (uint8_t)((value >> 8) & 0xffu);
}

static uint16_t read_u16_le(const uint8_t *bytes, size_t offset)
{
    return (uint16_t)((uint16_t)bytes[offset] | ((uint16_t)bytes[offset + 1] << 8));
}

static void write_u32_le(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset + 0] = (uint8_t)(value & 0xffu);
    bytes[offset + 1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[offset + 2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[offset + 3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t read_u32_le(const uint8_t *bytes, size_t offset)
{
    return (uint32_t)bytes[offset]
        | ((uint32_t)bytes[offset + 1] << 8)
        | ((uint32_t)bytes[offset + 2] << 16)
        | ((uint32_t)bytes[offset + 3] << 24);
}

static int read_entire_file(const char *path, uint8_t **bytes, size_t *byte_count)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;

    *bytes = NULL;
    *byte_count = 0;
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return 0;
    }
    rewind(file);
    data = (uint8_t *)malloc((size_t)size == 0 ? 1 : (size_t)size);
    if (data == NULL) {
        fclose(file);
        return 0;
    }
    if (size > 0 && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *bytes = data;
    *byte_count = (size_t)size;
    return 1;
}

static int write_entire_file(const char *path, const uint8_t *bytes, size_t byte_count)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    if (byte_count > 0 && fwrite(bytes, 1, byte_count, file) != byte_count) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static int append_byte(uint8_t **bytes, size_t *count, size_t *capacity, uint8_t value)
{
    if (*count == *capacity) {
        size_t next_capacity = *capacity == 0 ? 256 : *capacity * 2;
        uint8_t *next = (uint8_t *)realloc(*bytes, next_capacity);
        if (next == NULL) {
            return 0;
        }
        *bytes = next;
        *capacity = next_capacity;
    }
    (*bytes)[(*count)++] = value;
    return 1;
}

static int append_length_bit(uint8_t **bytes, uint32_t *bit_count, size_t *capacity, uint8_t bit)
{
    uint32_t byte_index = *bit_count / 8u;

    if (byte_index >= *capacity) {
        size_t next_capacity = *capacity == 0 ? 32 : *capacity * 2;
        uint8_t *next = (uint8_t *)realloc(*bytes, next_capacity);
        if (next == NULL) {
            return 0;
        }
        memset(next + *capacity, 0, next_capacity - *capacity);
        *bytes = next;
        *capacity = next_capacity;
    }

    set_bit_msb_first(*bytes, *bit_count, bit);
    ++(*bit_count);
    return 1;
}

static int append_bits(uint8_t **bytes, uint32_t *bit_count, size_t *capacity, uint32_t value, uint8_t bits)
{
    for (uint8_t i = 0; i < bits; ++i) {
        uint8_t bit = (uint8_t)((value >> (bits - 1u - i)) & 1u);
        if (!append_length_bit(bytes, bit_count, capacity, bit)) {
            return 0;
        }
    }
    return 1;
}

static int append_length_code(uint8_t **bytes, uint32_t *bit_count, size_t *capacity, uint32_t extra_length)
{
    if (extra_length <= 3) {
        return append_bits(bytes, bit_count, capacity, extra_length, 3);
    }
    if (extra_length <= 19) {
        if (!append_bits(bytes, bit_count, capacity, 2, 2)) {
            return 0;
        }
        return append_bits(bytes, bit_count, capacity, extra_length - 4u, 4);
    }
    if (extra_length <= WIF_RANS14_EXTRA_MAX) {
        if (!append_bits(bytes, bit_count, capacity, 6, 3)) {
            return 0;
        }
        return append_bits(bytes, bit_count, capacity, extra_length - 20u, 8);
    }
    return 0;
}

static int append_run(
    uint8_t **symbols,
    size_t *symbol_count,
    size_t *symbol_capacity,
    uint8_t **length_data,
    uint32_t *length_bit_count,
    size_t *length_capacity,
    uint8_t repeated_bit,
    uint32_t run_length
)
{
    while (run_length >= WIF_RANS14_RUN_BASE) {
        uint32_t chunk = run_length > WIF_RANS14_RUN_MAX ? WIF_RANS14_RUN_MAX : run_length;
        uint32_t extra_length = chunk - WIF_RANS14_RUN_BASE;

        if (!append_byte(symbols, symbol_count, symbol_capacity, (uint8_t)WIF_RANS14_RUN_SYMBOL) ||
            !append_byte(symbols, symbol_count, symbol_capacity, repeated_bit) ||
            !append_length_code(length_data, length_bit_count, length_capacity, extra_length)) {
            return 0;
        }

        run_length -= chunk;
    }

    for (uint32_t i = 0; i < run_length; ++i) {
        if (!append_byte(symbols, symbol_count, symbol_capacity, repeated_bit)) {
            return 0;
        }
    }
    return 1;
}

static int build_symbol_and_length_streams(
    const uint8_t *route_map,
    uint32_t bit_count,
    uint8_t **symbols,
    size_t *symbol_count,
    uint8_t **length_data,
    uint32_t *length_bit_count
)
{
    size_t symbol_capacity = 0;
    size_t length_capacity = 0;
    uint32_t i = 0;

    *symbols = NULL;
    *symbol_count = 0;
    *length_data = NULL;
    *length_bit_count = 0;

    while (i < bit_count) {
        uint8_t bit = get_bit_msb_first(route_map, i);
        uint32_t run_length = 1;

        while (i + run_length < bit_count && get_bit_msb_first(route_map, i + run_length) == bit) {
            ++run_length;
        }

        if (run_length >= WIF_RANS14_RUN_BASE) {
            if (!append_run(symbols, symbol_count, &symbol_capacity, length_data, length_bit_count, &length_capacity, bit, run_length)) {
                return 0;
            }
        } else {
            for (uint32_t j = 0; j < run_length; ++j) {
                if (!append_byte(symbols, symbol_count, &symbol_capacity, bit)) {
                    return 0;
                }
            }
        }
        i += run_length;
    }
    return 1;
}

static void build_frequencies(const uint8_t *symbols, size_t symbol_count, uint16_t *freq, uint32_t *cum)
{
    uint32_t counts[WIF_RANS14_SYMBOL_COUNT] = {0};
    uint32_t sum = 0;

    memset(freq, 0, sizeof(uint16_t) * WIF_RANS14_SYMBOL_COUNT);
    for (size_t i = 0; i < symbol_count; ++i) {
        counts[symbols[i]]++;
    }
    for (uint32_t s = 0; s < WIF_RANS14_SYMBOL_COUNT; ++s) {
        if (counts[s] > 0) {
            uint32_t value = (uint32_t)(((uint64_t)counts[s] * WIF_RANS14_SCALE) / symbol_count);
            freq[s] = (uint16_t)(value == 0 ? 1 : value);
            sum += freq[s];
        }
    }
    while (sum < WIF_RANS14_SCALE) {
        uint32_t best = 0;
        for (uint32_t s = 1; s < WIF_RANS14_SYMBOL_COUNT; ++s) {
            if (counts[s] > counts[best]) {
                best = s;
            }
        }
        freq[best]++;
        sum++;
    }
    while (sum > WIF_RANS14_SCALE) {
        uint32_t best = WIF_RANS14_SYMBOL_COUNT;
        for (uint32_t s = 0; s < WIF_RANS14_SYMBOL_COUNT; ++s) {
            if (freq[s] > 1 && (best == WIF_RANS14_SYMBOL_COUNT || freq[s] > freq[best])) {
                best = s;
            }
        }
        if (best == WIF_RANS14_SYMBOL_COUNT) {
            break;
        }
        freq[best]--;
        sum--;
    }
    cum[0] = 0;
    for (uint32_t s = 0; s < WIF_RANS14_SYMBOL_COUNT; ++s) {
        cum[s + 1] = cum[s] + freq[s];
    }
}

static int encode_symbols(
    const uint8_t *symbols,
    size_t symbol_count,
    const uint16_t *freq,
    const uint32_t *cum,
    uint32_t *final_state,
    uint8_t **stream,
    size_t *stream_size
)
{
    uint32_t state = WIF_RANS14_BYTE_L;
    uint8_t *renorm = NULL;
    size_t renorm_count = 0;
    size_t renorm_capacity = 0;

    for (size_t remaining = symbol_count; remaining > 0; --remaining) {
        uint8_t symbol = symbols[remaining - 1];
        uint32_t symbol_freq = freq[symbol];
        uint32_t start = cum[symbol];
        uint64_t x_max = ((uint64_t)(WIF_RANS14_BYTE_L >> WIF_RANS14_SCALE_BITS) << 8) * symbol_freq;

        while ((uint64_t)state >= x_max) {
            if (!append_byte(&renorm, &renorm_count, &renorm_capacity, (uint8_t)(state & 0xffu))) {
                free(renorm);
                return 0;
            }
            state >>= 8;
        }
        state = ((state / symbol_freq) << WIF_RANS14_SCALE_BITS) + (state % symbol_freq) + start;
    }

    *stream = (uint8_t *)malloc(renorm_count == 0 ? 1 : renorm_count);
    if (*stream == NULL) {
        free(renorm);
        return 0;
    }
    for (size_t i = 0; i < renorm_count; ++i) {
        (*stream)[i] = renorm[renorm_count - 1u - i];
    }
    free(renorm);
    *final_state = state;
    *stream_size = renorm_count;
    return 1;
}

static void build_decode_table(const uint16_t *freq, const uint32_t *cum, uint8_t *decode_table)
{
    for (uint32_t s = 0; s < WIF_RANS14_SYMBOL_COUNT; ++s) {
        for (uint32_t i = 0; i < freq[s]; ++i) {
            decode_table[cum[s] + i] = (uint8_t)s;
        }
    }
}

static int decode_symbols(
    const uint8_t *stream,
    size_t stream_size,
    uint32_t symbol_count,
    const uint16_t *freq,
    const uint32_t *cum,
    uint32_t final_state,
    uint8_t *symbols
)
{
    uint8_t decode_table[WIF_RANS14_SCALE];
    uint32_t state = final_state;
    size_t stream_pos = 0;

    build_decode_table(freq, cum, decode_table);
    for (uint32_t i = 0; i < symbol_count; ++i) {
        uint32_t cf = state & (WIF_RANS14_SCALE - 1u);
        uint8_t symbol = decode_table[cf];
        uint32_t symbol_freq = freq[symbol];
        uint32_t start = cum[symbol];

        symbols[i] = symbol;
        state = symbol_freq * (state >> WIF_RANS14_SCALE_BITS) + (cf - start);
        while (state < WIF_RANS14_BYTE_L && stream_pos < stream_size) {
            state = (state << 8) | stream[stream_pos++];
        }
    }
    return 1;
}

static void build_decode_table(const uint16_t *freq, const uint32_t *cum, uint8_t *decode_table);

static uint8_t decode_one_symbol(
    const uint8_t *stream,
    size_t stream_size,
    size_t *stream_pos,
    const uint16_t *freq,
    const uint32_t *cum,
    const uint8_t *decode_table,
    uint32_t *state
)
{
    uint32_t cf = *state & (WIF_RANS14_SCALE - 1u);
    uint8_t symbol = decode_table[cf];
    uint32_t symbol_freq = freq[symbol];
    uint32_t start = cum[symbol];

    *state = symbol_freq * (*state >> WIF_RANS14_SCALE_BITS) + (cf - start);
    while (*state < WIF_RANS14_BYTE_L && *stream_pos < stream_size) {
        *state = (*state << 8) | stream[(*stream_pos)++];
    }

    return symbol;
}

static int read_bits(const uint8_t *bytes, uint32_t bit_count, uint32_t *position, uint8_t count, uint32_t *value)
{
    uint32_t result = 0;

    if (*position + count > bit_count) {
        return 0;
    }
    for (uint8_t i = 0; i < count; ++i) {
        result = (result << 1) | get_bit_msb_first(bytes, (*position)++);
    }
    *value = result;
    return 1;
}

static int read_length_code(const uint8_t *length_data, uint32_t length_bit_count, uint32_t *position, uint32_t *extra_length)
{
    uint32_t first;
    uint32_t payload;

    if (!read_bits(length_data, length_bit_count, position, 1, &first)) {
        return 0;
    }
    if (first == 0) {
        if (!read_bits(length_data, length_bit_count, position, 2, &payload)) {
            return 0;
        }
        *extra_length = payload;
        return 1;
    }

    if (!read_bits(length_data, length_bit_count, position, 1, &first)) {
        return 0;
    }
    if (first == 0) {
        if (!read_bits(length_data, length_bit_count, position, 4, &payload)) {
            return 0;
        }
        *extra_length = 4u + payload;
        return 1;
    }

    if (!read_bits(length_data, length_bit_count, position, 1, &first) || first != 0) {
        return 0;
    }
    if (!read_bits(length_data, length_bit_count, position, 8, &payload)) {
        return 0;
    }
    *extra_length = 20u + payload;
    return 1;
}

static int expand_symbols(
    const uint8_t *symbols,
    uint32_t symbol_count,
    const uint8_t *length_data,
    uint32_t length_bit_count,
    uint8_t *route_map,
    uint32_t bit_count
)
{
    uint32_t out_bit = 0;
    uint32_t length_pos = 0;

    for (uint32_t i = 0; i < symbol_count; ++i) {
        uint8_t symbol = symbols[i];

        if (symbol == 0 || symbol == 1) {
            if (out_bit >= bit_count) {
                return 0;
            }
            set_bit_msb_first(route_map, out_bit++, symbol);
        } else if (symbol == WIF_RANS14_RUN_SYMBOL) {
            uint8_t repeated_bit;
            uint32_t extra_length;
            uint32_t run_length;

            if (i + 1 >= symbol_count) {
                return 0;
            }
            repeated_bit = symbols[++i];
            if (repeated_bit > 1 || !read_length_code(length_data, length_bit_count, &length_pos, &extra_length)) {
                return 0;
            }
            run_length = WIF_RANS14_RUN_BASE + extra_length;
            if (out_bit + run_length > bit_count) {
                return 0;
            }
            for (uint32_t j = 0; j < run_length; ++j) {
                set_bit_msb_first(route_map, out_bit++, repeated_bit);
            }
        } else {
            return 0;
        }
    }
    return out_bit == bit_count;
}

static int decode_route_map_until_complete(
    const uint8_t *rans_stream,
    size_t rans_stream_size,
    const uint8_t *length_data,
    uint32_t length_bit_count,
    const uint16_t *freq,
    const uint32_t *cum,
    uint32_t final_state,
    uint8_t *route_map,
    uint32_t bit_count
)
{
    uint8_t decode_table[WIF_RANS14_SCALE];
    uint32_t state = final_state;
    uint32_t out_bit = 0;
    uint32_t length_pos = 0;
    size_t stream_pos = 0;

    build_decode_table(freq, cum, decode_table);

    while (out_bit < bit_count) {
        uint8_t symbol = decode_one_symbol(rans_stream, rans_stream_size, &stream_pos, freq, cum, decode_table, &state);

        if (symbol == 0 || symbol == 1) {
            set_bit_msb_first(route_map, out_bit++, symbol);
        } else if (symbol == WIF_RANS14_RUN_SYMBOL) {
            uint8_t repeated_bit = decode_one_symbol(rans_stream, rans_stream_size, &stream_pos, freq, cum, decode_table, &state);
            uint32_t extra_length;
            uint32_t run_length;

            if (repeated_bit > 1 || !read_length_code(length_data, length_bit_count, &length_pos, &extra_length)) {
                return 0;
            }
            run_length = WIF_RANS14_RUN_BASE + extra_length;
            if (out_bit + run_length > bit_count) {
                return 0;
            }
            for (uint32_t i = 0; i < run_length; ++i) {
                set_bit_msb_first(route_map, out_bit++, repeated_bit);
            }
        } else {
            return 0;
        }
    }

    return 1;
}

int wif_rans_route_map_encode_file(
    const char *input_path,
    uint32_t bit_count,
    const char *output_path,
    char *error_message,
    size_t error_message_size
)
{
    uint8_t *input = NULL;
    uint8_t *symbols = NULL;
    uint8_t *length_data = NULL;
    uint8_t *rans_stream = NULL;
    uint8_t *output = NULL;
    size_t input_size = 0;
    size_t symbol_count = 0;
    uint32_t length_bit_count = 0;
    size_t rans_stream_size = 0;
    uint32_t input_byte_count;
    uint16_t freq[WIF_RANS14_SYMBOL_COUNT];
    uint32_t cum[WIF_RANS14_SYMBOL_COUNT + 1];
    uint32_t final_state;
    size_t length_byte_count;
    size_t output_size;
    size_t offset = 0;
    int ok = 0;

    if (input_path == NULL || output_path == NULL || bit_count == 0) {
        set_error(error_message, error_message_size, "invalid rANS v1.4 encode argument");
        return 0;
    }
    input_byte_count = ceil_div8_u32(bit_count);
    if (!read_entire_file(input_path, &input, &input_size) || input_size < input_byte_count) {
        set_error(error_message, error_message_size, "failed to read route map input");
        goto cleanup;
    }
    if (!build_symbol_and_length_streams(input, bit_count, &symbols, &symbol_count, &length_data, &length_bit_count)) {
        set_error(error_message, error_message_size, "failed to build v1.4 streams");
        goto cleanup;
    }
    if (symbol_count > UINT32_MAX) {
        set_error(error_message, error_message_size, "too many v1.4 symbols");
        goto cleanup;
    }

    build_frequencies(symbols, symbol_count, freq, cum);
    if (!encode_symbols(symbols, symbol_count, freq, cum, &final_state, &rans_stream, &rans_stream_size)) {
        set_error(error_message, error_message_size, "failed to rANS encode v1.4 symbols");
        goto cleanup;
    }
    if (rans_stream_size > UINT32_MAX) {
        set_error(error_message, error_message_size, "v1.4 rANS stream too large");
        goto cleanup;
    }

    length_byte_count = ceil_div8_u32(length_bit_count);
    output_size = 8 + 4 * 4 + WIF_RANS14_SYMBOL_COUNT * 2 + rans_stream_size + length_byte_count;
    output = (uint8_t *)malloc(output_size);
    if (output == NULL) {
        set_error(error_message, error_message_size, "failed to allocate v1.4 output");
        goto cleanup;
    }

    memcpy(output + offset, WIF_RANS14_MAGIC, 8);
    offset += 8;
    write_u32_le(output, offset, bit_count);
    offset += 4;
    write_u32_le(output, offset, length_bit_count);
    offset += 4;
    write_u32_le(output, offset, final_state);
    offset += 4;
    write_u32_le(output, offset, (uint32_t)rans_stream_size);
    offset += 4;
    for (uint32_t s = 0; s < WIF_RANS14_SYMBOL_COUNT; ++s) {
        write_u16_le(output, offset, freq[s]);
        offset += 2;
    }
    memcpy(output + offset, rans_stream, rans_stream_size);
    offset += rans_stream_size;
    if (length_byte_count > 0) {
        memcpy(output + offset, length_data, length_byte_count);
    }

    if (!write_entire_file(output_path, output, output_size)) {
        set_error(error_message, error_message_size, "failed to write v1.4 output");
        goto cleanup;
    }
    ok = 1;

cleanup:
    free(input);
    free(symbols);
    free(length_data);
    free(rans_stream);
    free(output);
    return ok;
}

int wif_rans_route_map_decode_file(
    const char *input_path,
    const char *output_path,
    char *error_message,
    size_t error_message_size
)
{
    uint8_t *input = NULL;
    uint8_t *route_map = NULL;
    size_t input_size = 0;
    size_t offset = 0;
    uint32_t bit_count;
    uint32_t route_byte_count;
    uint32_t length_bit_count;
    uint32_t final_state;
    uint32_t rans_stream_size;
    uint32_t length_byte_count;
    uint16_t freq[WIF_RANS14_SYMBOL_COUNT];
    uint32_t cum[WIF_RANS14_SYMBOL_COUNT + 1];
    const uint8_t *rans_stream;
    const uint8_t *length_data;
    int ok = 0;

    if (input_path == NULL || output_path == NULL) {
        set_error(error_message, error_message_size, "invalid rANS v1.4 decode argument");
        return 0;
    }
    if (!read_entire_file(input_path, &input, &input_size) || input_size < 8 + 4 * 4 + WIF_RANS14_SYMBOL_COUNT * 2) {
        set_error(error_message, error_message_size, "failed to read v1.4 input");
        goto cleanup;
    }
    if (memcmp(input, WIF_RANS14_MAGIC, 8) != 0) {
        set_error(error_message, error_message_size, "invalid v1.4 magic");
        goto cleanup;
    }

    offset = 8;
    bit_count = read_u32_le(input, offset);
    offset += 4;
    length_bit_count = read_u32_le(input, offset);
    offset += 4;
    final_state = read_u32_le(input, offset);
    offset += 4;
    rans_stream_size = read_u32_le(input, offset);
    offset += 4;
    route_byte_count = ceil_div8_u32(bit_count);
    length_byte_count = ceil_div8_u32(length_bit_count);

    cum[0] = 0;
    for (uint32_t s = 0; s < WIF_RANS14_SYMBOL_COUNT; ++s) {
        freq[s] = read_u16_le(input, offset);
        offset += 2;
        cum[s + 1] = cum[s] + freq[s];
    }

    if (bit_count == 0 ||
        cum[WIF_RANS14_SYMBOL_COUNT] != WIF_RANS14_SCALE ||
        input_size != offset + rans_stream_size + length_byte_count) {
        set_error(error_message, error_message_size, "invalid v1.4 header");
        goto cleanup;
    }

    rans_stream = input + offset;
    length_data = input + offset + rans_stream_size;
    route_map = (uint8_t *)calloc(route_byte_count, 1);
    if (route_map == NULL) {
        set_error(error_message, error_message_size, "failed to allocate v1.4 decode buffers");
        goto cleanup;
    }

    if (!decode_route_map_until_complete(rans_stream, rans_stream_size, length_data, length_bit_count, freq, cum, final_state, route_map, bit_count)) {
        set_error(error_message, error_message_size, "failed to decode v1.4 route map");
        goto cleanup;
    }
    if (!write_entire_file(output_path, route_map, route_byte_count)) {
        set_error(error_message, error_message_size, "failed to write decoded v1.4 route map");
        goto cleanup;
    }
    ok = 1;

cleanup:
    free(input);
    free(route_map);
    return ok;
}

int wif_rans_route_map_decode_to_memory(
    const char *input_path,
    uint8_t **route_map,
    uint32_t *route_bit_count,
    uint32_t *route_byte_count,
    char *error_message,
    size_t error_message_size
)
{
    uint8_t *input = NULL;
    uint8_t *decoded = NULL;
    size_t input_size = 0;
    size_t offset = 0;
    uint32_t bit_count;
    uint32_t decoded_byte_count;
    uint32_t length_bit_count;
    uint32_t final_state;
    uint32_t rans_stream_size;
    uint32_t length_byte_count;
    uint16_t freq[WIF_RANS14_SYMBOL_COUNT];
    uint32_t cum[WIF_RANS14_SYMBOL_COUNT + 1];
    const uint8_t *rans_stream;
    const uint8_t *length_data;
    int ok = 0;

    if (input_path == NULL || route_map == NULL || route_bit_count == NULL || route_byte_count == NULL) {
        set_error(error_message, error_message_size, "invalid rANS v1.4 memory decode argument");
        return 0;
    }

    *route_map = NULL;
    *route_bit_count = 0;
    *route_byte_count = 0;

    if (!read_entire_file(input_path, &input, &input_size) || input_size < 8 + 4 * 4 + WIF_RANS14_SYMBOL_COUNT * 2) {
        set_error(error_message, error_message_size, "failed to read v1.4 input");
        goto cleanup;
    }
    if (memcmp(input, WIF_RANS14_MAGIC, 8) != 0) {
        set_error(error_message, error_message_size, "invalid v1.4 magic");
        goto cleanup;
    }

    offset = 8;
    bit_count = read_u32_le(input, offset);
    offset += 4;
    length_bit_count = read_u32_le(input, offset);
    offset += 4;
    final_state = read_u32_le(input, offset);
    offset += 4;
    rans_stream_size = read_u32_le(input, offset);
    offset += 4;
    decoded_byte_count = ceil_div8_u32(bit_count);
    length_byte_count = ceil_div8_u32(length_bit_count);

    cum[0] = 0;
    for (uint32_t s = 0; s < WIF_RANS14_SYMBOL_COUNT; ++s) {
        freq[s] = read_u16_le(input, offset);
        offset += 2;
        cum[s + 1] = cum[s] + freq[s];
    }

    if (bit_count == 0 ||
        cum[WIF_RANS14_SYMBOL_COUNT] != WIF_RANS14_SCALE ||
        input_size != offset + rans_stream_size + length_byte_count) {
        set_error(error_message, error_message_size, "invalid v1.4 header");
        goto cleanup;
    }

    decoded = (uint8_t *)calloc(decoded_byte_count, 1);
    if (decoded == NULL) {
        set_error(error_message, error_message_size, "failed to allocate v1.4 route map");
        goto cleanup;
    }

    rans_stream = input + offset;
    length_data = input + offset + rans_stream_size;
    if (!decode_route_map_until_complete(rans_stream, rans_stream_size, length_data, length_bit_count, freq, cum, final_state, decoded, bit_count)) {
        set_error(error_message, error_message_size, "failed to decode v1.4 route map");
        goto cleanup;
    }

    *route_map = decoded;
    *route_bit_count = bit_count;
    *route_byte_count = decoded_byte_count;
    decoded = NULL;
    ok = 1;

cleanup:
    free(input);
    free(decoded);
    return ok;
}

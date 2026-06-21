#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define RANS_L (1u << 23)
#define RANS_SCALE_BITS 12
#define RANS_TOTAL (1u << RANS_SCALE_BITS)
#define MAX_BLOCKS 20000
#define STREAM_COUNT 8

typedef struct {
    uint8_t *data;
    size_t count;
    size_t capacity;
} ByteBuffer;

typedef struct {
    wchar_t name[MAX_PATH];
    int row;
    int column;
    size_t header_offset;
    size_t header_length;
    size_t position_absolute_offset;
    size_t position_absolute_length;
    size_t position_abs_delta_offset;
    size_t position_abs_delta_length;
    size_t position_sign_delta_offset;
    size_t position_sign_delta_length;
    size_t fcolor_offset;
    size_t fcolor_length;
    size_t dcolor_offset;
    size_t dcolor_length;
    size_t background_color_offset;
    size_t background_color_length;
    size_t line_color_offset;
    size_t line_color_length;
    int has_lines;
    int background_color;
    int line_color;
    int line_color_bits;
} BlockEntry;

typedef struct {
    int round;
    int type;
    int start_x;
    int start_y;
    int length;
    uint8_t colors[32];
} ParsedLine;

typedef struct {
    ByteBuffer raw;
    ByteBuffer meta;
    ByteBuffer payload;
    uint16_t frequencies[256];
    uint16_t cumulative[256];
    uint32_t counts[256];
    int entry_count;
    int verified;
} EncodedStream;

typedef struct {
    int line_color_bits;
    int first_background_color;
    int first_line_color;
    int previous_background_color;
    int previous_line_color;
    int has_first_color;
} GlobalMeta;

static int compare_blocks(const void *left, const void *right) {
    const BlockEntry *a = (const BlockEntry *)left;
    const BlockEntry *b = (const BlockEntry *)right;
    if (a->row != b->row) return a->row - b->row;
    return a->column - b->column;
}

static int append_byte(ByteBuffer *buffer, uint8_t value) {
    if (buffer->count == buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity * 2;
        uint8_t *data = (uint8_t *)realloc(buffer->data, capacity);
        if (!data) return 0;
        buffer->data = data;
        buffer->capacity = capacity;
    }
    buffer->data[buffer->count++] = value;
    return 1;
}

static int append_bytes(ByteBuffer *buffer, const uint8_t *data, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (!append_byte(buffer, data[i])) return 0;
    }
    return 1;
}

static int append_u16(ByteBuffer *buffer, uint16_t value) {
    return append_byte(buffer, (uint8_t)value) &&
        append_byte(buffer, (uint8_t)(value >> 8));
}

static int append_u32(ByteBuffer *buffer, uint32_t value) {
    return append_byte(buffer, (uint8_t)value) &&
        append_byte(buffer, (uint8_t)(value >> 8)) &&
        append_byte(buffer, (uint8_t)(value >> 16)) &&
        append_byte(buffer, (uint8_t)(value >> 24));
}

static int zigzag_encode_delta(int delta) {
    return delta >= 0 ? delta * 2 - (delta != 0) : -delta * 2;
}

static int append_palette_delta(ByteBuffer *stream, int previous, int current) {
    const int encoded = zigzag_encode_delta(current - previous);
    if (encoded < 255) {
        return append_byte(stream, (uint8_t)encoded);
    }
    return append_byte(stream, 255) && append_byte(stream, (uint8_t)current);
}

static char *read_text(const wchar_t *path) {
    FILE *file = NULL;
    long size;
    char *text;
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) return NULL;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    text = (char *)malloc((size_t)size + 1);
    if (!text || fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[size] = '\0';
    fclose(file);
    return text;
}

static const char *find_key(const char *start, const char *limit, const char *key) {
    const char *p = start;
    const size_t key_length = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        if (limit && p >= limit) return NULL;
        return p + key_length;
    }
    return NULL;
}

static int parse_int_after_key(const char *start, const char *limit, const char *key, int *value) {
    const char *p = find_key(start, limit, key);
    char *end;
    if (!p) return 0;
    while (*p == ' ' || *p == ':') p++;
    *value = (int)strtol(p, &end, 10);
    return end != p && (!limit || end <= limit);
}

static int parse_orientation_type(const char *object_start, const char *object_end, int *type) {
    const char *p = find_key(object_start, object_end, "\"orientation\": \"");
    if (!p) return 0;
    if (strncmp(p, "vertical", 8) == 0) {
        *type = 0;
        return 1;
    }
    if (strncmp(p, "horizontal", 10) == 0) {
        *type = 1;
        return 1;
    }
    if (strncmp(p, "dot", 3) == 0) {
        *type = 2;
        return 1;
    }
    return 0;
}

static int parse_encoded_symbols(
    const char *object_start,
    const char *object_end,
    ParsedLine *line
) {
    const char *p = find_key(object_start, object_end, "\"encodedSymbols\": [");
    int index = 0;
    if (!p) return 0;
    while (p < object_end && *p && *p != ']') {
        char *end;
        long value;
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == ']') break;
        value = strtol(p, &end, 10);
        if (end == p || value < 0 || value > 255) return 0;
        if (index >= 2) {
            if (index - 2 >= (int)(sizeof(line->colors) / sizeof(line->colors[0]))) return 0;
            line->colors[index - 2] = (uint8_t)value;
        }
        index++;
        p = end;
    }
    return index == line->length + 2;
}

static int parse_lines(const char *json, ParsedLine **lines_out, int *line_count_out) {
    const char *lines_start = strstr(json, "\"lines\": [");
    const char *lines_buffer = strstr(json, "\"linesBuffer\"");
    const char *cursor;
    ParsedLine *lines = NULL;
    int count = 0;
    int capacity = 32;
    if (!lines_start || !lines_buffer || lines_start > lines_buffer) return 0;
    cursor = lines_start;
    lines = (ParsedLine *)malloc((size_t)capacity * sizeof(*lines));
    if (!lines) return 0;
    while ((cursor = strstr(cursor, "{\"lineId\"")) != NULL && cursor < lines_buffer) {
        const char *object_end = strstr(cursor, "]}");
        ParsedLine *line;
        if (!object_end || object_end > lines_buffer) break;
        object_end += 2;
        if (count == capacity) {
            ParsedLine *expanded;
            capacity *= 2;
            expanded = (ParsedLine *)realloc(lines, (size_t)capacity * sizeof(*lines));
            if (!expanded) {
                free(lines);
                return 0;
            }
            lines = expanded;
        }
        line = &lines[count];
        memset(line, 0, sizeof(*line));
        if (!parse_orientation_type(cursor, object_end, &line->type) ||
            !parse_int_after_key(cursor, object_end, "\"round\"", &line->round) ||
            !parse_int_after_key(cursor, object_end, "\"length\"", &line->length) ||
            !parse_int_after_key(cursor, object_end, "\"x\"", &line->start_x) ||
            !parse_int_after_key(cursor, object_end, "\"y\"", &line->start_y) ||
            line->length < 1 ||
            line->length > (int)(sizeof(line->colors) / sizeof(line->colors[0])) ||
            !parse_encoded_symbols(cursor, object_end, line)) {
            free(lines);
            return 0;
        }
        count++;
        cursor = object_end;
    }
    *lines_out = lines;
    *line_count_out = count;
    return 1;
}

static int minimum_length_for_round(int round) {
    if (round == 1) return 4;
    if (round == 2) return 3;
    if (round == 3) return 2;
    return 1;
}

static int append_split_streams(
    const ParsedLine *lines,
    int line_count,
    ByteBuffer *header_stream,
    ByteBuffer *position_absolute_stream,
    ByteBuffer *position_abs_delta_stream,
    ByteBuffer *position_sign_delta_stream,
    ByteBuffer *fcolor_stream,
    ByteBuffer *dcolor_stream
) {
    int group_start = 0;
    while (group_start < line_count) {
        const ParsedLine *first = &lines[group_start];
        const int minimum_length = minimum_length_for_round(first->round);
        int group_end = group_start + 1;
        int has_extra_length = minimum_length == 4;
        int line_index;
        while (group_end < line_count &&
               lines[group_end].round == first->round &&
               lines[group_end].type == first->type) {
            group_end++;
        }
        if (!append_byte(header_stream, (uint8_t)minimum_length) ||
            !append_byte(header_stream, (uint8_t)(group_end - group_start)) ||
            !append_byte(header_stream, (uint8_t)first->type)) {
            return 0;
        }
        for (line_index = group_start; line_index < group_end; line_index++) {
            const ParsedLine *line = &lines[line_index];
            int abs_delta = 0;
            int sign_delta = 0;
            if (line_index > group_start) {
                const ParsedLine *previous = &lines[line_index - 1];
                if (first->type == 0) {
                    abs_delta = line->start_x - previous->start_x;
                    sign_delta = line->start_y - previous->start_y;
                } else if (first->type == 1) {
                    abs_delta = line->start_y - previous->start_y;
                    sign_delta = line->start_x - previous->start_x;
                } else {
                    abs_delta = line->start_x - previous->start_x;
                    sign_delta = line->start_y - previous->start_y;
                }
                if (abs_delta < 0) abs_delta = -abs_delta;
            }
            if (has_extra_length &&
                !append_byte(position_absolute_stream, (uint8_t)(line->length - minimum_length))) {
                return 0;
            }
            if (line_index == group_start) {
                if (!append_byte(position_absolute_stream, (uint8_t)line->start_x) ||
                    !append_byte(position_absolute_stream, (uint8_t)line->start_y)) {
                    return 0;
                }
            } else if (!append_byte(position_abs_delta_stream, (uint8_t)abs_delta) ||
                       !append_byte(position_sign_delta_stream, (uint8_t)zigzag_encode_delta(sign_delta))) {
                return 0;
            }
            if (!append_byte(fcolor_stream, line->colors[0]) ||
                !append_bytes(dcolor_stream, line->colors + 1, (size_t)(line->length - 1))) {
                return 0;
            }
        }
        group_start = group_end;
    }
    return 1;
}

static int collect_all_blocks(
    const wchar_t *root,
    EncodedStream streams[STREAM_COUNT],
    BlockEntry blocks[MAX_BLOCKS],
    int *block_count_out,
    GlobalMeta *global_meta
) {
    WIN32_FIND_DATAW find_data;
    HANDLE handle;
    wchar_t pattern[MAX_PATH];
    int count = 0;
    int index;
    if (swprintf_s(pattern, MAX_PATH, L"%ls\\block_*_*", root) < 0) return 0;
    handle = FindFirstFileW(pattern, &find_data);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    do {
        int row;
        int column;
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (swscanf_s(find_data.cFileName, L"block_%d_%d", &row, &column) != 2) continue;
        if (count >= MAX_BLOCKS) {
            FindClose(handle);
            return 0;
        }
        blocks[count].row = row;
        blocks[count].column = column;
        wcsncpy_s(blocks[count].name, MAX_PATH, find_data.cFileName, _TRUNCATE);
        count++;
    } while (FindNextFileW(handle, &find_data));
    FindClose(handle);
    qsort(blocks, count, sizeof(blocks[0]), compare_blocks);
    {
        int valid_count = 0;
        for (index = 0; index < count; index++) {
            wchar_t json_path[MAX_PATH];
            char *json;
            ParsedLine *lines = NULL;
            int line_count = 0;
            int line_color = 0;
            int background_color = 0;
            int line_color_bits = 0;
            if (swprintf_s(
                    json_path, MAX_PATH, L"%ls\\%ls\\horizontal_vertical_lines.json",
                    root, blocks[index].name
                ) < 0) {
                return 0;
            }
            json = read_text(json_path);
            if (!json) continue;
            if (!parse_lines(json, &lines, &line_count)) {
                free(json);
                return 0;
            }
            parse_int_after_key(json, NULL, "\"lineColor\"", &line_color);
            parse_int_after_key(json, NULL, "\"backgroundColor\"", &background_color);
            parse_int_after_key(json, NULL, "\"lineColorBits\"", &line_color_bits);
            free(json);
            if (valid_count != index) blocks[valid_count] = blocks[index];
            blocks[valid_count].has_lines = line_count > 0;
            blocks[valid_count].line_color = line_color;
            blocks[valid_count].background_color = background_color;
            blocks[valid_count].line_color_bits = line_color_bits;
            if (line_color_bits > 0 && global_meta->line_color_bits == 0) {
                global_meta->line_color_bits = line_color_bits;
            }
            blocks[valid_count].header_offset = streams[0].raw.count;
            blocks[valid_count].position_absolute_offset = streams[1].raw.count;
            blocks[valid_count].position_abs_delta_offset = streams[2].raw.count;
            blocks[valid_count].position_sign_delta_offset = streams[3].raw.count;
            blocks[valid_count].fcolor_offset = streams[4].raw.count;
            blocks[valid_count].dcolor_offset = streams[5].raw.count;
            blocks[valid_count].background_color_offset = streams[6].raw.count;
            blocks[valid_count].line_color_offset = streams[7].raw.count;
            if (!append_split_streams(
                    lines, line_count,
                    &streams[0].raw, &streams[1].raw, &streams[2].raw, &streams[3].raw,
                    &streams[4].raw, &streams[5].raw
                )) {
                free(lines);
                return 0;
            }
            if (line_count > 0) {
                if (!global_meta->has_first_color) {
                    global_meta->first_background_color = background_color;
                    global_meta->first_line_color = line_color;
                    global_meta->previous_background_color = background_color;
                    global_meta->previous_line_color = line_color;
                    global_meta->has_first_color = 1;
                } else {
                    if (!append_palette_delta(&streams[6].raw, global_meta->previous_background_color, background_color) ||
                        !append_palette_delta(&streams[7].raw, global_meta->previous_line_color, line_color)) {
                        free(lines);
                        return 0;
                    }
                    global_meta->previous_background_color = background_color;
                    global_meta->previous_line_color = line_color;
                }
            }
            free(lines);
            blocks[valid_count].header_length =
                streams[0].raw.count - blocks[valid_count].header_offset;
            blocks[valid_count].position_absolute_length =
                streams[1].raw.count - blocks[valid_count].position_absolute_offset;
            blocks[valid_count].position_abs_delta_length =
                streams[2].raw.count - blocks[valid_count].position_abs_delta_offset;
            blocks[valid_count].position_sign_delta_length =
                streams[3].raw.count - blocks[valid_count].position_sign_delta_offset;
            blocks[valid_count].fcolor_length =
                streams[4].raw.count - blocks[valid_count].fcolor_offset;
            blocks[valid_count].dcolor_length =
                streams[5].raw.count - blocks[valid_count].dcolor_offset;
            if (!append_byte(&streams[0].raw, 0)) {
                return 0;
            }
            blocks[valid_count].header_length =
                streams[0].raw.count - blocks[valid_count].header_offset;
            blocks[valid_count].background_color_length =
                streams[6].raw.count - blocks[valid_count].background_color_offset;
            blocks[valid_count].line_color_length =
                streams[7].raw.count - blocks[valid_count].line_color_offset;
            valid_count++;
        }
        *block_count_out = valid_count;
    }
    return 1;
}

static void normalize_stream(EncodedStream *stream) {
    int symbol;
    int total = 0;
    memset(stream->counts, 0, sizeof(stream->counts));
    memset(stream->frequencies, 0, sizeof(stream->frequencies));
    stream->entry_count = 0;
    for (size_t i = 0; i < stream->raw.count; i++) stream->counts[stream->raw.data[i]]++;
    for (symbol = 0; symbol < 256; symbol++) {
        if (stream->counts[symbol]) {
            stream->frequencies[symbol] =
                (uint16_t)(((uint64_t)stream->counts[symbol] * RANS_TOTAL) / stream->raw.count);
            if (!stream->frequencies[symbol]) stream->frequencies[symbol] = 1;
            total += stream->frequencies[symbol];
            stream->entry_count++;
        }
    }
    while (total < RANS_TOTAL) {
        int best = 0;
        for (symbol = 1; symbol < 256; symbol++) {
            if (stream->counts[symbol] > stream->counts[best]) best = symbol;
        }
        stream->frequencies[best]++;
        total++;
    }
    while (total > RANS_TOTAL) {
        int best = -1;
        for (symbol = 0; symbol < 256; symbol++) {
            if (stream->frequencies[symbol] > 1 &&
                (best < 0 || stream->frequencies[symbol] > stream->frequencies[best])) {
                best = symbol;
            }
        }
        stream->frequencies[best]--;
        total--;
    }
    total = 0;
    for (symbol = 0; symbol < 256; symbol++) {
        stream->cumulative[symbol] = (uint16_t)total;
        total += stream->frequencies[symbol];
    }
}

static int encode_stream(EncodedStream *stream) {
    ByteBuffer emitted = {0};
    uint32_t state = RANS_L;
    size_t i;
    if (stream->raw.count == 0) return 1;
    for (i = stream->raw.count; i > 0; i--) {
        int symbol = stream->raw.data[i - 1];
        uint32_t frequency = stream->frequencies[symbol];
        uint32_t maximum_state = ((RANS_L >> RANS_SCALE_BITS) << 8) * frequency;
        while (state >= maximum_state) {
            if (!append_byte(&emitted, (uint8_t)state)) goto fail;
            state >>= 8;
        }
        state = (state / frequency) * RANS_TOTAL +
            stream->cumulative[symbol] + state % frequency;
    }
    for (i = 0; i < 4; i++) {
        if (!append_byte(&stream->payload, (uint8_t)(state >> (i * 8)))) goto fail;
    }
    while (emitted.count) {
        if (!append_byte(&stream->payload, emitted.data[--emitted.count])) goto fail;
    }
    free(emitted.data);
    return 1;
fail:
    free(emitted.data);
    return 0;
}

static int decode_verify_stream(const EncodedStream *stream) {
    uint32_t state;
    size_t cursor = 4;
    size_t i;
    if (stream->raw.count == 0) return stream->payload.count == 0;
    if (stream->payload.count < 4) return 0;
    state = stream->payload.data[0] |
        ((uint32_t)stream->payload.data[1] << 8) |
        ((uint32_t)stream->payload.data[2] << 16) |
        ((uint32_t)stream->payload.data[3] << 24);
    for (i = 0; i < stream->raw.count; i++) {
        uint32_t slot = state & (RANS_TOTAL - 1);
        int symbol;
        for (symbol = 0; symbol < 256; symbol++) {
            if (stream->frequencies[symbol] &&
                slot >= stream->cumulative[symbol] &&
                slot < (uint32_t)stream->cumulative[symbol] + stream->frequencies[symbol]) {
                break;
            }
        }
        if (symbol == 256 || symbol != stream->raw.data[i]) return 0;
        state = stream->frequencies[symbol] * (state >> RANS_SCALE_BITS) +
            slot - stream->cumulative[symbol];
        while (state < RANS_L && cursor < stream->payload.count) {
            state = (state << 8) | stream->payload.data[cursor++];
        }
    }
    return 1;
}

static int build_stream_meta(EncodedStream *stream, uint8_t stream_id) {
    int symbol;
    if (!append_byte(&stream->meta, stream_id) ||
        !append_u32(&stream->meta, (uint32_t)stream->raw.count) ||
        !append_u32(&stream->meta, (uint32_t)stream->payload.count) ||
        !append_byte(&stream->meta, RANS_SCALE_BITS) ||
        !append_u16(&stream->meta, (uint16_t)stream->entry_count)) {
        return 0;
    }
    for (symbol = 0; symbol < 256; symbol++) {
        if (stream->frequencies[symbol]) {
            if (!append_byte(&stream->meta, (uint8_t)symbol) ||
                !append_u16(&stream->meta, stream->frequencies[symbol])) {
                return 0;
            }
        }
    }
    return 1;
}

static int write_binary(
    const wchar_t *path,
    EncodedStream streams[STREAM_COUNT],
    const GlobalMeta *global_meta
) {
    FILE *file = NULL;
    ByteBuffer header = {0};
    if (!append_byte(&header, 'R') || !append_byte(&header, 'L') ||
        !append_byte(&header, 'B') || !append_byte(&header, 'S') ||
        !append_byte(&header, 12) || !append_byte(&header, STREAM_COUNT) ||
        !append_byte(&header, (uint8_t)global_meta->line_color_bits) ||
        !append_byte(&header, (uint8_t)global_meta->first_background_color) ||
        !append_byte(&header, (uint8_t)global_meta->first_line_color)) {
        free(header.data);
        return 0;
    }
    for (int i = 0; i < STREAM_COUNT; i++) {
        if (!append_bytes(&header, streams[i].meta.data, streams[i].meta.count)) {
            free(header.data);
            return 0;
        }
    }
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) {
        free(header.data);
        return 0;
    }
    if (fwrite(header.data, 1, header.count, file) != header.count) {
        fclose(file);
        free(header.data);
        return 0;
    }
    for (int i = 0; i < STREAM_COUNT; i++) {
        if (fwrite(streams[i].payload.data, 1, streams[i].payload.count, file) !=
            streams[i].payload.count) {
            fclose(file);
            free(header.data);
            return 0;
        }
    }
    fclose(file);
    free(header.data);
    return 1;
}

static size_t file_header_size(const EncodedStream streams[STREAM_COUNT]) {
    size_t size = 9;
    int i;
    for (i = 0; i < STREAM_COUNT; i++) size += streams[i].meta.count;
    return size;
}

static int write_report(
    const wchar_t *path,
    const EncodedStream streams[STREAM_COUNT],
    const BlockEntry blocks[MAX_BLOCKS],
    int block_count,
    const GlobalMeta *global_meta
) {
    FILE *file = NULL;
    size_t payload_size = 0;
    int stream_index;
    const char *stream_names[STREAM_COUNT] = {
        "header", "positionAbsolute", "positionAbsDelta", "positionSignDelta",
        "Fcolor", "Dcolor", "backgroundColor", "lineColor"
    };
    for (stream_index = 0; stream_index < STREAM_COUNT; stream_index++) {
        payload_size += streams[stream_index].payload.count;
    }
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) return 0;
    fprintf(file, "{\n");
    fprintf(file, "  \"codec\": \"rANS_v1.2_split_header_position3_fcolor_dcolor_block_end_palette_delta\",\n");
    fprintf(file, "  \"blockCount\": %d,\n", block_count);
    fprintf(file, "  \"streamCount\": %d,\n", STREAM_COUNT);
    fprintf(file, "  \"lineColorBits\": %d,\n", global_meta->line_color_bits);
    fprintf(file, "  \"firstBackgroundColor\": %d,\n", global_meta->first_background_color);
    fprintf(file, "  \"firstLineColor\": %d,\n", global_meta->first_line_color);
    fprintf(file, "  \"blockEndHeaderSymbol\": 0,\n");
    fprintf(file, "  \"fileHeaderSizeBytes\": %zu,\n", file_header_size(streams));
    fprintf(file, "  \"payloadSizeBytes\": %zu,\n", payload_size);
    fprintf(file, "  \"totalCompressedSizeBytes\": %zu,\n", file_header_size(streams) + payload_size);
    fprintf(file, "  \"streams\": {\n");
    for (stream_index = 0; stream_index < STREAM_COUNT; stream_index++) {
        fprintf(file, "    \"%s\": {\"rawSizeBytes\": %zu, \"metaSizeBytes\": %zu, "
            "\"payloadSizeBytes\": %zu, \"frequencyEntryCount\": %d, \"decodeVerified\": %s}%s\n",
            stream_names[stream_index],
            streams[stream_index].raw.count,
            streams[stream_index].meta.count,
            streams[stream_index].payload.count,
            streams[stream_index].entry_count,
            streams[stream_index].verified ? "true" : "false",
            stream_index + 1 == STREAM_COUNT ? "" : ",");
    }
    fprintf(file, "  },\n");
    fprintf(file, "  \"blocks\": [\n");
    for (int i = 0; i < block_count; i++) {
        fprintf(
            file,
            "    {\"name\": \"%ls\", "
            "\"hasLines\": %s, "
            "\"backgroundColor\": %d, "
            "\"lineColor\": %d, "
            "\"lineColorBits\": %d, "
            "\"header\": {\"offset\": %zu, \"length\": %zu}, "
            "\"positionAbsolute\": {\"offset\": %zu, \"length\": %zu}, "
            "\"positionAbsDelta\": {\"offset\": %zu, \"length\": %zu}, "
            "\"positionSignDelta\": {\"offset\": %zu, \"length\": %zu}, "
            "\"Fcolor\": {\"offset\": %zu, \"length\": %zu}, "
            "\"Dcolor\": {\"offset\": %zu, \"length\": %zu}, "
            "\"backgroundColorStream\": {\"offset\": %zu, \"length\": %zu}, "
            "\"lineColorStream\": {\"offset\": %zu, \"length\": %zu}}%s\n",
            blocks[i].name,
            blocks[i].has_lines ? "true" : "false",
            blocks[i].background_color,
            blocks[i].line_color,
            blocks[i].line_color_bits,
            blocks[i].header_offset, blocks[i].header_length,
            blocks[i].position_absolute_offset, blocks[i].position_absolute_length,
            blocks[i].position_abs_delta_offset, blocks[i].position_abs_delta_length,
            blocks[i].position_sign_delta_offset, blocks[i].position_sign_delta_length,
            blocks[i].fcolor_offset, blocks[i].fcolor_length,
            blocks[i].dcolor_offset, blocks[i].dcolor_length,
            blocks[i].background_color_offset, blocks[i].background_color_length,
            blocks[i].line_color_offset, blocks[i].line_color_length,
            i + 1 == block_count ? "" : ","
        );
    }
    fprintf(file, "  ]\n");
    fprintf(file, "}\n");
    fclose(file);
    return 1;
}

static void free_streams(EncodedStream streams[STREAM_COUNT]) {
    for (int i = 0; i < STREAM_COUNT; i++) {
        free(streams[i].raw.data);
        free(streams[i].meta.data);
        free(streams[i].payload.data);
    }
}

int wmain(int argc, wchar_t **argv) {
    EncodedStream streams[STREAM_COUNT];
    GlobalMeta global_meta;
    BlockEntry *blocks;
    int block_count = 0;
    wchar_t binary_path[MAX_PATH];
    wchar_t report_path[MAX_PATH];
    memset(streams, 0, sizeof(streams));
    memset(&global_meta, 0, sizeof(global_meta));
    if (argc < 2 || argc > 4) {
        fwprintf(stderr, L"Usage: %ls all-block-output-root [output.rans] [report.json]\n", argv[0]);
        return 1;
    }
    blocks = (BlockEntry *)calloc(MAX_BLOCKS, sizeof(*blocks));
    if (!blocks || !collect_all_blocks(argv[1], streams, blocks, &block_count, &global_meta)) {
        fprintf(stderr, "Failed to collect/split block line data\n");
        free(blocks);
        free_streams(streams);
        return 1;
    }
    for (int i = 0; i < STREAM_COUNT; i++) {
        normalize_stream(&streams[i]);
        if (!encode_stream(&streams[i])) {
            fprintf(stderr, "Failed to encode stream %d\n", i);
            free(blocks);
            free_streams(streams);
            return 1;
        }
        streams[i].verified = decode_verify_stream(&streams[i]);
        if (!build_stream_meta(&streams[i], (uint8_t)i)) {
            fprintf(stderr, "Failed to build stream metadata\n");
            free(blocks);
            free_streams(streams);
            return 1;
        }
    }
    if (argc >= 3) wcsncpy_s(binary_path, MAX_PATH, argv[2], _TRUNCATE);
    else swprintf_s(binary_path, MAX_PATH, L"%ls\\all_lines_buffer_v1_2.rans", argv[1]);
    if (argc >= 4) wcsncpy_s(report_path, MAX_PATH, argv[3], _TRUNCATE);
    else swprintf_s(report_path, MAX_PATH, L"%ls\\all_lines_buffer_v1_2_rans_report.json", argv[1]);
    if (!write_binary(binary_path, streams, &global_meta) ||
        !write_report(report_path, streams, blocks, block_count, &global_meta)) {
        fprintf(stderr, "Failed to write output\n");
        free(blocks);
        free_streams(streams);
        return 1;
    }
    printf("Blocks: %d\n", block_count);
    printf("Header stream raw/payload: %zu / %zu bytes\n",
        streams[0].raw.count, streams[0].payload.count);
    printf("PositionAbsolute stream raw/payload: %zu / %zu bytes\n",
        streams[1].raw.count, streams[1].payload.count);
    printf("PositionAbsDelta stream raw/payload: %zu / %zu bytes\n",
        streams[2].raw.count, streams[2].payload.count);
    printf("PositionSignDelta stream raw/payload: %zu / %zu bytes\n",
        streams[3].raw.count, streams[3].payload.count);
    printf("Fcolor stream raw/payload: %zu / %zu bytes\n",
        streams[4].raw.count, streams[4].payload.count);
    printf("Dcolor stream raw/payload: %zu / %zu bytes\n",
        streams[5].raw.count, streams[5].payload.count);
    printf("BackgroundColor stream raw/payload: %zu / %zu bytes\n",
        streams[6].raw.count, streams[6].payload.count);
    printf("LineColor stream raw/payload: %zu / %zu bytes\n",
        streams[7].raw.count, streams[7].payload.count);
    printf("File header: %zu bytes\n", file_header_size(streams));
    {
        size_t payload = 0;
        int i;
        for (i = 0; i < STREAM_COUNT; i++) payload += streams[i].payload.count;
        printf("Payload: %zu bytes\n", payload);
        printf("Total compressed: %zu bytes\n", file_header_size(streams) + payload);
    }
    printf("Decode verified: %s\n",
        streams[0].verified && streams[1].verified &&
        streams[2].verified && streams[3].verified &&
        streams[4].verified && streams[5].verified &&
        streams[6].verified && streams[7].verified ? "yes" : "no");
    wprintf(L"Binary: %ls\n", binary_path);
    wprintf(L"Report: %ls\n", report_path);
    free(blocks);
    free_streams(streams);
    return 0;
}

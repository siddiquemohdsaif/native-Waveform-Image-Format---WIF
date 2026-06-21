#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define RANS_L (1u << 23)
#define EXPECTED_STREAMS 8
#define BLOCK_SIZE 16

typedef struct {
    uint8_t stream_id;
    uint32_t raw_size;
    uint32_t payload_size;
    uint8_t scale_bits;
    uint16_t entry_count;
    uint16_t frequencies[256];
    uint16_t cumulative[256];
    const uint8_t *payload;
    uint8_t *raw;
} DecodedStream;

typedef struct {
    int line_color_bits;
    int first_background_color;
    int first_line_color;
} GlobalMeta;

static uint32_t read_u32_le(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint16_t read_u16_le(const uint8_t *data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static int zigzag_decode(int value) {
    return (value & 1) ? ((value + 1) / 2) : -(value / 2);
}

static int read_palette_color(DecodedStream *stream, size_t *cursor, int previous, int *current) {
    int symbol;
    if (*cursor >= stream->raw_size) return 0;
    symbol = stream->raw[(*cursor)++];
    if (symbol == 255) {
        if (*cursor >= stream->raw_size) return 0;
        *current = stream->raw[(*cursor)++];
    } else {
        *current = previous + zigzag_decode(symbol);
    }
    return *current >= 0 && *current <= 255;
}

static uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)((299 * r + 587 * g + 114 * b + 500) / 1000);
}

static int color_to_code(int value, int line_color, int background_color, int max_code) {
    const int range = background_color - line_color;
    int numerator;
    int quantized;
    if (range == 0) return 0;
    if (range > 0) {
        if (value <= line_color) return 0;
        if (value >= background_color) return max_code;
        numerator = (value - line_color) * max_code;
        quantized = (numerator + range / 2) / range;
    } else {
        const int positive_range = -range;
        if (value >= line_color) return 0;
        if (value <= background_color) return max_code;
        numerator = (line_color - value) * max_code;
        quantized = (numerator + positive_range / 2) / positive_range;
    }
    if (quantized < 0) return 0;
    if (quantized > max_code) return max_code;
    return quantized;
}

static int color_from_code(int code, int line_color, int background_color, int max_code) {
    int reconstructed;
    if (max_code <= 0) return line_color;
    reconstructed = line_color +
        ((background_color - line_color) * code +
         (background_color >= line_color ? max_code / 2 : -(max_code / 2))) / max_code;
    if (reconstructed < 0) reconstructed = 0;
    if (reconstructed > 255) reconstructed = 255;
    return reconstructed;
}

static void color_code_range(int code, int line_color, int background_color, int max_code, int *range_min, int *range_max) {
    const int minimum_color = line_color < background_color ? line_color : background_color;
    const int maximum_color = line_color > background_color ? line_color : background_color;
    *range_min = 256;
    *range_max = -1;
    for (int value = minimum_color; value <= maximum_color; value++) {
        if (color_to_code(value, line_color, background_color, max_code) == code) {
            if (value < *range_min) *range_min = value;
            if (value > *range_max) *range_max = value;
        }
    }
    if (*range_max < 0) {
        int decoded = color_from_code(code, line_color, background_color, max_code);
        *range_min = decoded;
        *range_max = decoded;
    }
}

static void smooth_decode_codes(uint8_t decoded[BLOCK_SIZE], const int codes[BLOCK_SIZE], int length, int line_color, int background_color, int max_code) {
    int run_start = 0;
    while (run_start < length) {
        const int code = codes[run_start];
        int run_end = run_start + 1;
        int range_min, range_max, start_value, end_value, run_length;
        while (run_end < length && codes[run_end] == code) run_end++;
        run_length = run_end - run_start;
        color_code_range(code, line_color, background_color, max_code, &range_min, &range_max);
        if (run_start == 0 && run_end == length) {
            uint8_t midpoint = (uint8_t)((range_min + range_max + 1) / 2);
            for (int index = 0; index < run_length; index++) decoded[index] = midpoint;
            break;
        }
        start_value = range_max;
        end_value = range_min;
        if (run_start > 0) {
            int previous_min, previous_max;
            color_code_range(codes[run_start - 1], line_color, background_color, max_code, &previous_min, &previous_max);
            start_value = previous_min + previous_max > range_min + range_max ? range_max : range_min;
        } else if (run_end < length) {
            int next_min, next_max;
            color_code_range(codes[run_end], line_color, background_color, max_code, &next_min, &next_max);
            start_value = next_min + next_max > range_min + range_max ? range_min : range_max;
        }
        if (run_end < length) {
            int next_min, next_max;
            color_code_range(codes[run_end], line_color, background_color, max_code, &next_min, &next_max);
            end_value = next_min + next_max > range_min + range_max ? range_max : range_min;
        } else if (run_start > 0) {
            end_value = start_value == range_min ? range_max : range_min;
        }
        for (int index = 0; index < run_length; index++) {
            int value;
            if (run_length == 1) {
                value = (range_min + range_max + 1) / 2;
            } else if (start_value == end_value) {
                int opposite_value = start_value == range_min ? range_max : range_min;
                int denominator = run_length - 1;
                int edge_weight = abs(2 * index - denominator);
                value = opposite_value + ((start_value - opposite_value) * edge_weight +
                    (start_value >= opposite_value ? denominator / 2 : -denominator / 2)) / denominator;
            } else {
                value = start_value + ((end_value - start_value) * index +
                    (end_value >= start_value ? (run_length - 1) / 2 : -(run_length - 1) / 2)) / (run_length - 1);
            }
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            decoded[run_start + index] = (uint8_t)value;
        }
        run_start = run_end;
    }
}

static int read_file(const wchar_t *path, uint8_t **data, size_t *size) {
    FILE *file = NULL;
    long length;
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) return 0;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return 0; }
    *data = (uint8_t *)malloc((size_t)length);
    if (!*data) { fclose(file); return 0; }
    if (fread(*data, 1, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(*data);
        *data = NULL;
        return 0;
    }
    fclose(file);
    *size = (size_t)length;
    return 1;
}

static int load_png_y(const wchar_t *path, uint8_t **pixels_out, uint32_t *width_out, uint32_t *height_out) {
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    uint8_t *bgra = NULL;
    uint8_t *y = NULL;
    UINT width = 0, height = 0;
    HRESULT result;
    result = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(result)) goto fail;
    result = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(result)) goto fail;
    result = IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(result)) goto fail;
    bgra = (uint8_t *)malloc((size_t)width * height * 4);
    y = (uint8_t *)malloc((size_t)width * height);
    if (!bgra || !y) goto fail;
    result = IWICFormatConverter_CopyPixels(converter, NULL, width * 4, width * height * 4, bgra);
    if (FAILED(result)) goto fail;
    for (uint32_t i = 0; i < width * height; i++) y[i] = rgb_to_y(bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4]);
    free(bgra);
    IWICFormatConverter_Release(converter);
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(decoder);
    IWICImagingFactory_Release(factory);
    *pixels_out = y;
    *width_out = width;
    *height_out = height;
    return 1;
fail:
    free(bgra);
    free(y);
    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (factory) IWICImagingFactory_Release(factory);
    return 0;
}

static int write_png_y(const wchar_t *path, const uint8_t *pixels, uint32_t width, uint32_t height) {
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    uint8_t *bgr = (uint8_t *)malloc((size_t)width * height * 3);
    HRESULT result;
    if (!bgr) return 0;
    for (uint32_t i = 0; i < width * height; i++) bgr[i * 3] = bgr[i * 3 + 1] = bgr[i * 3 + 2] = pixels[i];
    result = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(result)) goto fail;
    result = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_Initialize(frame, properties);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_SetSize(frame, width, height);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_WritePixels(frame, height, width * 3, width * height * 3, bgr);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(result)) goto fail;
    free(bgr);
    if (properties) IPropertyBag2_Release(properties);
    IWICBitmapFrameEncode_Release(frame);
    IWICBitmapEncoder_Release(encoder);
    IWICStream_Release(stream);
    IWICImagingFactory_Release(factory);
    return 1;
fail:
    free(bgr);
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    return 0;
}

static int parse_rans_file(const uint8_t *file, size_t file_size, DecodedStream streams[EXPECTED_STREAMS], GlobalMeta *meta) {
    size_t offset = 0;
    if (file_size < 9 || file[0] != 'R' || file[1] != 'L' || file[2] != 'B' || file[3] != 'S') return 0;
    if (file[4] != 12 || file[5] != EXPECTED_STREAMS) return 0;
    meta->line_color_bits = file[6];
    meta->first_background_color = file[7];
    meta->first_line_color = file[8];
    offset = 9;
    memset(streams, 0, sizeof(DecodedStream) * EXPECTED_STREAMS);
    for (int i = 0; i < EXPECTED_STREAMS; i++) {
        int total = 0;
        if (offset + 12 > file_size) return 0;
        streams[i].stream_id = file[offset++];
        streams[i].raw_size = read_u32_le(file + offset); offset += 4;
        streams[i].payload_size = read_u32_le(file + offset); offset += 4;
        streams[i].scale_bits = file[offset++];
        streams[i].entry_count = read_u16_le(file + offset); offset += 2;
        for (int entry = 0; entry < streams[i].entry_count; entry++) {
            uint8_t symbol;
            uint16_t frequency;
            if (offset + 3 > file_size) return 0;
            symbol = file[offset++];
            frequency = read_u16_le(file + offset); offset += 2;
            streams[i].frequencies[symbol] = frequency;
        }
        for (int symbol = 0; symbol < 256; symbol++) {
            streams[i].cumulative[symbol] = (uint16_t)total;
            total += streams[i].frequencies[symbol];
        }
    }
    for (int i = 0; i < EXPECTED_STREAMS; i++) {
        if (offset + streams[i].payload_size > file_size) return 0;
        streams[i].payload = file + offset;
        offset += streams[i].payload_size;
    }
    return offset == file_size;
}

static int decode_stream(DecodedStream *stream) {
    uint32_t state;
    size_t cursor = 4;
    if (stream->raw_size == 0) return stream->payload_size == 0;
    if (stream->payload_size < 4) return 0;
    stream->raw = (uint8_t *)malloc(stream->raw_size);
    if (!stream->raw) return 0;
    state = read_u32_le(stream->payload);
    for (uint32_t i = 0; i < stream->raw_size; i++) {
        uint32_t slot = state & ((1u << stream->scale_bits) - 1);
        int symbol;
        for (symbol = 0; symbol < 256; symbol++) {
            if (stream->frequencies[symbol] && slot >= stream->cumulative[symbol] &&
                slot < (uint32_t)stream->cumulative[symbol] + stream->frequencies[symbol]) break;
        }
        if (symbol == 256) return 0;
        stream->raw[i] = (uint8_t)symbol;
        state = stream->frequencies[symbol] * (state >> stream->scale_bits) + slot - stream->cumulative[symbol];
        while (state < RANS_L && cursor < stream->payload_size) state = (state << 8) | stream->payload[cursor++];
    }
    return 1;
}

static int apply_decoded_lines(uint8_t *image, uint32_t width, uint32_t height, DecodedStream streams[EXPECTED_STREAMS], const GlobalMeta *meta) {
    size_t header_cursor = 0, position_absolute_cursor = 0, position_abs_delta_cursor = 0;
    size_t position_sign_delta_cursor = 0, fcolor_cursor = 0, dcolor_cursor = 0;
    size_t background_cursor = 0, line_cursor = 0;
    int previous_background = meta->first_background_color;
    int previous_line = meta->first_line_color;
    int saw_non_empty_block = 0;
    uint32_t blocks_x = width / BLOCK_SIZE, blocks_y = height / BLOCK_SIZE;
    int max_code = (1 << meta->line_color_bits) - 1;
    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            int block_has_line = 0;
            int background_color = previous_background;
            int line_color = previous_line;
            while (1) {
                int min_length, line_count, type;
                int group_has_extra = 0;
                int previous_x = 0, previous_y = 0;
                if (header_cursor >= streams[0].raw_size) return 0;
                min_length = streams[0].raw[header_cursor++];
                if (min_length == 0) break;
                if (header_cursor + 2 > streams[0].raw_size) return 0;
                line_count = streams[0].raw[header_cursor++];
                type = streams[0].raw[header_cursor++];
                group_has_extra = (min_length == 4);
                if (!block_has_line) {
                    block_has_line = 1;
                    if (!saw_non_empty_block) {
                        saw_non_empty_block = 1;
                    } else {
                        if (!read_palette_color(&streams[6], &background_cursor, previous_background, &background_color) ||
                            !read_palette_color(&streams[7], &line_cursor, previous_line, &line_color)) {
                            return 0;
                        }
                        previous_background = background_color;
                        previous_line = line_color;
                    }
                }
                for (int line_index = 0; line_index < line_count; line_index++) {
                    int length = min_length;
                    int start_x, start_y;
                    int codes[BLOCK_SIZE];
                    uint8_t decoded[BLOCK_SIZE];
                    if (group_has_extra) {
                        if (position_absolute_cursor >= streams[1].raw_size) return 0;
                        length += streams[1].raw[position_absolute_cursor++];
                    }
                    if (fcolor_cursor >= streams[4].raw_size) return 0;
                    if (line_index == 0) {
                        if (position_absolute_cursor + 2 > streams[1].raw_size) return 0;
                        start_x = streams[1].raw[position_absolute_cursor++];
                        start_y = streams[1].raw[position_absolute_cursor++];
                    } else {
                        int abs_delta;
                        int sign_delta;
                        if (position_abs_delta_cursor >= streams[2].raw_size ||
                            position_sign_delta_cursor >= streams[3].raw_size) {
                            return 0;
                        }
                        abs_delta = streams[2].raw[position_abs_delta_cursor++];
                        sign_delta = zigzag_decode(streams[3].raw[position_sign_delta_cursor++]);
                        if (type == 0) {
                            start_x = previous_x + abs_delta;
                            start_y = previous_y + sign_delta;
                        } else if (type == 1) {
                            start_x = previous_x + sign_delta;
                            start_y = previous_y + abs_delta;
                        } else {
                            start_x = previous_x + abs_delta;
                            start_y = previous_y + sign_delta;
                        }
                    }
                    previous_x = start_x;
                    previous_y = start_y;
                    if (length < 1 || length > BLOCK_SIZE) return 0;
                    codes[0] = streams[4].raw[fcolor_cursor++];
                    for (int i = 1; i < length; i++) {
                        if (dcolor_cursor >= streams[5].raw_size) return 0;
                        codes[i] = codes[i - 1] + zigzag_decode(streams[5].raw[dcolor_cursor++]);
                        if (codes[i] < 0) codes[i] = 0;
                        if (codes[i] > max_code) codes[i] = max_code;
                    }
                    smooth_decode_codes(decoded, codes, length, line_color, background_color, max_code);
                    for (int i = 0; i < length; i++) {
                        int x = start_x, y = start_y;
                        if (type == 0) y += i;
                        else if (type == 1) x += i;
                        if (x >= 0 && x < BLOCK_SIZE && y >= 0 && y < BLOCK_SIZE)
                            image[(by * BLOCK_SIZE + y) * width + bx * BLOCK_SIZE + x] = decoded[i];
                    }
                }
            }
        }
    }
    return 1;
}

int wmain(int argc, wchar_t **argv) {
    uint8_t *residue = NULL, *rans_file = NULL;
    size_t rans_size = 0;
    uint32_t width = 0, height = 0;
    DecodedStream streams[EXPECTED_STREAMS];
    GlobalMeta meta;
    const wchar_t *output_path;
    HRESULT result;
    if (argc < 3 || argc > 4) {
        fwprintf(stderr, L"Usage: %ls full_residue.png all_lines_buffer_v1_2.rans [output.png]\n", argv[0]);
        return 1;
    }
    output_path = argc >= 4 ? argv[3] : L"full_residue_linesBuffer_decoded_current.png";
    result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) return 1;
    if (!load_png_y(argv[1], &residue, &width, &height) ||
        !read_file(argv[2], &rans_file, &rans_size) ||
        !parse_rans_file(rans_file, rans_size, streams, &meta)) {
        fprintf(stderr, "Failed input parse\n");
        CoUninitialize();
        return 1;
    }
    for (int i = 0; i < EXPECTED_STREAMS; i++) {
        if (!decode_stream(&streams[i])) {
            fprintf(stderr, "Failed stream decode %d\n", i);
            CoUninitialize();
            return 1;
        }
    }
    if (!apply_decoded_lines(residue, width, height, streams, &meta)) {
        fprintf(stderr, "Failed to apply decoded line buffer; writing partial output\n");
    }
    if (!write_png_y(output_path, residue, width, height)) {
        fprintf(stderr, "Failed output write\n");
        CoUninitialize();
        return 1;
    }
    wprintf(L"Output: %ls\n", output_path);
    for (int i = 0; i < EXPECTED_STREAMS; i++) free(streams[i].raw);
    free(residue);
    free(rans_file);
    CoUninitialize();
    return 0;
}

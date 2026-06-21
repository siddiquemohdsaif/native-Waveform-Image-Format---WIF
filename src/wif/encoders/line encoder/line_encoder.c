#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#define WIDTH 16
#define HEIGHT 16
#define PIXEL_COUNT (WIDTH * HEIGHT)
#define MAX_LINES PIXEL_COUNT
#define BAND_COUNT 16
#define BAND_SIZE 16
#define GRAPH_WIDTH 800
#define GRAPH_HEIGHT 420
#define RESIDUE_FILTER_ENABLED 0

static int g_line_color_bits = 4;

static int line_color_max_code(void) {
    return (1 << g_line_color_bits) - 1;
}

typedef struct {
    int index;
    int min_value;
    int max_value;
    int width;
    int count;
    double weight_percent;
    double peak_value;
} SpectralBand;

typedef struct {
    const char *orientation;
    int start_x;
    int start_y;
    int end_x;
    int end_y;
    int length;
    int width;
    int round;
    int first_pixel_position_from_origin;
    uint8_t color_values[HEIGHT];
} DetectedLine;

static uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)((299 * r + 587 * g + 114 * b + 500) / 1000);
}

static int compare_u8(const void *left, const void *right) {
    return (int)*(const uint8_t *)left - (int)*(const uint8_t *)right;
}

static int color_to_4bit(int value, int line_color, int background_color) {
    const int range = background_color - line_color;
    const int max_code = line_color_max_code();
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

static uint8_t color_from_4bit(int value, int line_color, int background_color) {
    const int max_code = line_color_max_code();
    int reconstructed = line_color +
        ((background_color - line_color) * value +
         (background_color >= line_color ? max_code / 2 : -(max_code / 2))) / max_code;
    if (reconstructed < 0) reconstructed = 0;
    if (reconstructed > 255) reconstructed = 255;
    return (uint8_t)reconstructed;
}

static void color_4bit_range(
    int code,
    int line_color,
    int background_color,
    int *range_min,
    int *range_max
) {
    const int minimum_color = line_color < background_color ? line_color : background_color;
    const int maximum_color = line_color > background_color ? line_color : background_color;
    int value;
    *range_min = 256;
    *range_max = -1;
    for (value = minimum_color; value <= maximum_color; value++) {
        if (color_to_4bit(value, line_color, background_color) == code) {
            if (value < *range_min) *range_min = value;
            if (value > *range_max) *range_max = value;
        }
    }
    if (*range_max < 0) {
        const int decoded = color_from_4bit(code, line_color, background_color);
        *range_min = decoded;
        *range_max = decoded;
    }
}

static void smooth_decode_4bit_line(
    uint8_t decoded[HEIGHT],
    const DetectedLine *line,
    int line_color,
    int background_color
) {
    int codes[HEIGHT];
    int run_start = 0;
    int index;
    for (index = 0; index < line->length; index++) {
        codes[index] = color_to_4bit(line->color_values[index], line_color, background_color);
    }
    while (run_start < line->length) {
        const int code = codes[run_start];
        int run_end = run_start + 1;
        int range_min;
        int range_max;
        int start_value;
        int end_value;
        int run_length;
        while (run_end < line->length && codes[run_end] == code) run_end++;
        run_length = run_end - run_start;
        color_4bit_range(code, line_color, background_color, &range_min, &range_max);

        /*
         * A line containing only one palette code has no surrounding direction.
         * Keep it at the palette midpoint instead of inventing an edge-to-edge
         * gradient across the full line.
         */
        if (run_start == 0 && run_end == line->length) {
            const uint8_t midpoint = (uint8_t)((range_min + range_max + 1) / 2);
            for (index = 0; index < run_length; index++) decoded[index] = midpoint;
            break;
        }

        start_value = range_max;
        end_value = range_min;
        if (run_start > 0) {
            int previous_min;
            int previous_max;
            int previous_center;
            color_4bit_range(codes[run_start - 1], line_color, background_color, &previous_min, &previous_max);
            previous_center = previous_min + previous_max;
            start_value = previous_center > range_min + range_max ? range_max : range_min;
        } else if (run_end < line->length) {
            int next_min;
            int next_max;
            color_4bit_range(codes[run_end], line_color, background_color, &next_min, &next_max);
            start_value = next_min + next_max > range_min + range_max ? range_min : range_max;
        }
        if (run_end < line->length) {
            int next_min;
            int next_max;
            color_4bit_range(codes[run_end], line_color, background_color, &next_min, &next_max);
            end_value = next_min + next_max > range_min + range_max ? range_max : range_min;
        } else if (run_start > 0) {
            end_value = start_value == range_min ? range_max : range_min;
        }

        for (index = 0; index < run_length; index++) {
            int value;
            if (run_length == 1) {
                value = (range_min + range_max + 1) / 2;
            } else if (start_value == end_value) {
                const int opposite_value = start_value == range_min ? range_max : range_min;
                const int denominator = run_length - 1;
                const int edge_weight = abs(2 * index - denominator);
                value = opposite_value +
                    ((start_value - opposite_value) * edge_weight +
                     (start_value >= opposite_value ? denominator / 2 : -denominator / 2)) /
                    denominator;
            } else {
                value = start_value +
                    ((end_value - start_value) * index +
                     (end_value >= start_value ? (run_length - 1) / 2 : -(run_length - 1) / 2)) /
                    (run_length - 1);
            }
            decoded[run_start + index] = (uint8_t)value;
        }
        run_start = run_end;
    }
}

static void redraw_lines_buffer(
    uint8_t output[PIXEL_COUNT],
    const uint8_t residue[PIXEL_COUNT],
    const DetectedLine lines[MAX_LINES],
    int line_count,
    int line_color,
    int background_color
) {
    int line_index;
    memcpy(output, residue, PIXEL_COUNT);
    for (line_index = 0; line_index < line_count; line_index++) {
        const DetectedLine *line = &lines[line_index];
        uint8_t decoded_colors[HEIGHT];
        int color_index;
        smooth_decode_4bit_line(decoded_colors, line, line_color, background_color);
        for (color_index = 0; color_index < line->length; color_index++) {
            int x = line->start_x;
            int y = line->start_y;
            if (strcmp(line->orientation, "vertical") == 0) y += color_index;
            else if (strcmp(line->orientation, "horizontal") == 0) x += color_index;
            output[y * WIDTH + x] = decoded_colors[color_index];
        }
    }
}

static int zigzag_encode_delta(int delta) {
    return delta >= 0 ? delta * 2 - (delta != 0) : -delta * 2;
}

static int minimum_length_for_round(int round) {
    if (round == 1) return 4;
    if (round == 2) return 3;
    if (round == 3) return 2;
    return 1;
}

static int orientation_type(const char *orientation) {
    if (strcmp(orientation, "vertical") == 0) return 0;
    if (strcmp(orientation, "horizontal") == 0) return 1;
    return 2;
}

static void analyze_spectrum(
    const uint8_t values[PIXEL_COUNT],
    SpectralBand bands[BAND_COUNT],
    double *energy_percent,
    double *energy_concentration
) {
    uint8_t band_values[BAND_COUNT][PIXEL_COUNT];
    int counts[BAND_COUNT] = {0};
    int min_value = 255;
    int max_value = 0;
    double ideal;
    double sum_deviation = 0.0;
    int i;

    for (i = 0; i < PIXEL_COUNT; i++) {
        const int value = values[i];
        const int band = value / BAND_SIZE;
        band_values[band][counts[band]++] = (uint8_t)value;
        if (value < min_value) min_value = value;
        if (value > max_value) max_value = value;
    }
    for (i = 0; i < BAND_COUNT; i++) {
        bands[i].index = i;
        bands[i].min_value = i * BAND_SIZE;
        bands[i].max_value = bands[i].min_value + BAND_SIZE - 1;
        bands[i].width = BAND_SIZE;
        bands[i].count = counts[i];
        bands[i].weight_percent = ((double)counts[i] * 100.0) / PIXEL_COUNT;
        bands[i].peak_value = 0.0;
        if (counts[i] > 0) {
            qsort(band_values[i], counts[i], sizeof(uint8_t), compare_u8);
            if (counts[i] & 1) {
                bands[i].peak_value = band_values[i][counts[i] / 2];
            } else {
                bands[i].peak_value =
                    (band_values[i][counts[i] / 2 - 1] +
                     band_values[i][counts[i] / 2]) / 2.0;
            }
        }
    }
    *energy_percent = ((double)(max_value - min_value) / 255.0) * 100.0;
    ideal = 100.0 / BAND_COUNT;
    for (i = 0; i < BAND_COUNT; i++) {
        sum_deviation += fabs(bands[i].weight_percent - ideal);
    }
    *energy_concentration = sum_deviation / (2.0 * (100.0 - ideal));
}

static void analyze_adaptive_spectrum(
    const uint8_t values[PIXEL_COUNT],
    SpectralBand bands[BAND_COUNT],
    double *energy_percent,
    double *energy_concentration
) {
    uint8_t band_values[BAND_COUNT][PIXEL_COUNT];
    int counts[BAND_COUNT] = {0};
    int min_value = 255;
    int max_value = 0;
    int range;
    int base_width;
    double ideal;
    double sum_deviation = 0.0;
    int i;

    for (i = 0; i < PIXEL_COUNT; i++) {
        if (values[i] < min_value) min_value = values[i];
        if (values[i] > max_value) max_value = values[i];
    }
    range = max_value - min_value;
    base_width = range / BAND_COUNT;

    for (i = 0; i < BAND_COUNT; i++) {
        bands[i].index = i;
        bands[i].min_value = min_value + i * base_width;
        bands[i].max_value = i == BAND_COUNT - 1
            ? max_value
            : bands[i].min_value + base_width - 1;
        bands[i].width = i == BAND_COUNT - 1
            ? range - base_width * (BAND_COUNT - 1)
            : base_width;
        if (bands[i].width < 0) bands[i].width = 0;
    }
    for (i = 0; i < PIXEL_COUNT; i++) {
        int band = base_width == 0 ? BAND_COUNT - 1 : (values[i] - min_value) / base_width;
        if (band >= BAND_COUNT) band = BAND_COUNT - 1;
        band_values[band][counts[band]++] = values[i];
    }
    for (i = 0; i < BAND_COUNT; i++) {
        bands[i].count = counts[i];
        bands[i].weight_percent = ((double)counts[i] * 100.0) / PIXEL_COUNT;
        bands[i].peak_value = 0.0;
        if (counts[i] > 0) {
            qsort(band_values[i], counts[i], sizeof(uint8_t), compare_u8);
            if (counts[i] & 1) {
                bands[i].peak_value = band_values[i][counts[i] / 2];
            } else {
                bands[i].peak_value =
                    (band_values[i][counts[i] / 2 - 1] +
                     band_values[i][counts[i] / 2]) / 2.0;
            }
        }
    }
    *energy_percent = ((double)range / 255.0) * 100.0;
    ideal = 100.0 / BAND_COUNT;
    for (i = 0; i < BAND_COUNT; i++) {
        sum_deviation += fabs(bands[i].weight_percent - ideal);
    }
    *energy_concentration = sum_deviation / (2.0 * (100.0 - ideal));
}

static void select_colors(
    const SpectralBand bands[BAND_COUNT],
    int *background_band,
    int *line_band,
    int *background_color,
    int *line_color
) {
    int first = 0;
    int second = -1;
    int i;
    for (i = 1; i < BAND_COUNT; i++) {
        if (bands[i].weight_percent > bands[first].weight_percent) {
            first = i;
        }
    }
    for (i = 0; i < BAND_COUNT; i++) {
        if (i == first || bands[i].count == 0) continue;
        if (second < 0 || abs(i - first) > abs(second - first)) {
            second = i;
        }
    }
    if (second < 0) second = first;
    *background_band = first;
    *line_band = second;
    *background_color = (int)(bands[first].peak_value + 0.5);
    *line_color = (int)(bands[second].peak_value + 0.5);
}

static int load_png_y_channel(const wchar_t *input_path, uint8_t y_values[PIXEL_COUNT]) {
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    HRESULT result;
    UINT width = 0;
    UINT height = 0;
    uint8_t bgra[PIXEL_COUNT * 4];
    int index;

    result = CoCreateInstance(
        &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory
    );
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateDecoderFromFilename(
        factory, input_path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder
    );
    if (FAILED(result)) goto fail;
    result = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    if (FAILED(result)) goto fail;
    if (width != WIDTH || height != HEIGHT) {
        fprintf(stderr, "Input must be exactly 16x16, got %ux%u\n", width, height);
        goto fail;
    }
    result = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(result)) goto fail;
    result = IWICFormatConverter_Initialize(
        converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom
    );
    if (FAILED(result)) goto fail;
    result = IWICFormatConverter_CopyPixels(
        converter, NULL, WIDTH * 4, sizeof(bgra), bgra
    );
    if (FAILED(result)) goto fail;

    for (index = 0; index < PIXEL_COUNT; index++) {
        const uint8_t b = bgra[index * 4 + 0];
        const uint8_t g = bgra[index * 4 + 1];
        const uint8_t r = bgra[index * 4 + 2];
        y_values[index] = rgb_to_y(r, g, b);
    }

    IWICFormatConverter_Release(converter);
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(decoder);
    IWICImagingFactory_Release(factory);
    return 1;

fail:
    fwprintf(stderr, L"Failed to decode PNG: %ls (HRESULT 0x%08lx)\n", input_path, result);
    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (factory) IWICImagingFactory_Release(factory);
    return 0;
}

static int write_y_plane_png(const wchar_t *output_path, const uint8_t y_plane[PIXEL_COUNT]) {
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    uint8_t bgr[PIXEL_COUNT * 3];
    HRESULT result;
    int index;

    for (index = 0; index < PIXEL_COUNT; index++) {
        bgr[index * 3 + 0] = y_plane[index];
        bgr[index * 3 + 1] = y_plane[index];
        bgr[index * 3 + 2] = y_plane[index];
    }

    result = CoCreateInstance(
        &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory
    );
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(result)) goto fail;
    result = IWICStream_InitializeFromFilename(stream, output_path, GENERIC_WRITE);
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_Initialize(frame, properties);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_SetSize(frame, WIDTH, HEIGHT);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_WritePixels(frame, HEIGHT, WIDTH * 3, sizeof(bgr), bgr);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(result)) goto fail;

    if (properties) IPropertyBag2_Release(properties);
    IWICBitmapFrameEncode_Release(frame);
    IWICBitmapEncoder_Release(encoder);
    IWICStream_Release(stream);
    IWICImagingFactory_Release(factory);
    return 1;

fail:
    fwprintf(stderr, L"Failed to write PNG: %ls (HRESULT 0x%08lx)\n", output_path, result);
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    return 0;
}

static void set_graph_rgb(uint8_t *pixels, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    int offset;
    if (x < 0 || x >= GRAPH_WIDTH || y < 0 || y >= GRAPH_HEIGHT) return;
    offset = (y * GRAPH_WIDTH + x) * 3;
    pixels[offset] = b;
    pixels[offset + 1] = g;
    pixels[offset + 2] = r;
}

static void fill_graph_rect(
    uint8_t *pixels, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b
) {
    int x;
    int y;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) set_graph_rgb(pixels, x, y, r, g, b);
    }
}

static int write_band_histogram_png(
    const wchar_t *output_path,
    const SpectralBand bands[BAND_COUNT]
) {
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    uint8_t *pixels = (uint8_t *)malloc(GRAPH_WIDTH * GRAPH_HEIGHT * 3);
    HRESULT result = E_FAIL;
    const int left = 55;
    const int baseline = 365;
    const int plot_height = 300;
    const int bar_width = 34;
    const int gap = 10;
    double maximum_weight = 1.0;
    int i;
    if (!pixels) return 0;
    memset(pixels, 255, GRAPH_WIDTH * GRAPH_HEIGHT * 3);
    for (i = 0; i < BAND_COUNT; i++) {
        if (bands[i].weight_percent > maximum_weight) maximum_weight = bands[i].weight_percent;
    }
    fill_graph_rect(pixels, left - 2, baseline - plot_height, left, baseline, 35, 35, 35);
    fill_graph_rect(pixels, left, baseline, left + BAND_COUNT * (bar_width + gap), baseline + 2, 35, 35, 35);
    for (i = 0; i < BAND_COUNT; i++) {
        const int x = left + 8 + i * (bar_width + gap);
        int height = (int)((bands[i].weight_percent / maximum_weight) * plot_height + 0.5);
        const uint8_t shade = (uint8_t)(40 + i * 12);
        if (bands[i].count > 0 && height < 1) height = 1;
        fill_graph_rect(pixels, x, baseline - height, x + bar_width - 1, baseline - 1, 40, shade, 215);
    }
    result = CoCreateInstance(
        &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory
    );
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(result)) goto fail;
    result = IWICStream_InitializeFromFilename(stream, output_path, GENERIC_WRITE);
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_Initialize(frame, properties);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_SetSize(frame, GRAPH_WIDTH, GRAPH_HEIGHT);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_WritePixels(
        frame, GRAPH_HEIGHT, GRAPH_WIDTH * 3, GRAPH_WIDTH * GRAPH_HEIGHT * 3, pixels
    );
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(result)) goto fail;
    result = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(result)) goto fail;
    free(pixels);
    if (properties) IPropertyBag2_Release(properties);
    IWICBitmapFrameEncode_Release(frame);
    IWICBitmapEncoder_Release(encoder);
    IWICStream_Release(stream);
    IWICImagingFactory_Release(factory);
    return 1;
fail:
    free(pixels);
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    return 0;
}

static int is_line_pixel(
    uint8_t value,
    int line_color,
    int background_color,
    int threshold_percent
) {
    const int maximum_contrast = abs(background_color - line_color);
    const int current_contrast = abs(background_color - (int)value);
    const int line_band = line_color / BAND_SIZE;
    const int pixel_band = value / BAND_SIZE;
    const int minimum_color = background_color < line_color ? background_color : line_color;
    const int maximum_color = background_color > line_color ? background_color : line_color;
    const int between_colors = value >= minimum_color && value <= maximum_color;
    const int in_line_band = pixel_band == line_band;
    (void)threshold_percent;
    if (maximum_contrast == 0) return 0;
    return (between_colors || in_line_band) &&
        current_contrast * 16 >= maximum_contrast;
}

static int detect_vertical_lines(
    uint8_t residue[PIXEL_COUNT],
    int line_color,
    int background_color,
    DetectedLine lines[MAX_LINES],
    int line_count,
    int minimum_length,
    int threshold_percent,
    int round
) {
    int x;
    for (x = 0; x < WIDTH; x++) {
        int y = 0;
        while (y < HEIGHT) {
            int start;
            int end;
            if (!is_line_pixel(
                    residue[y * WIDTH + x], line_color, background_color, threshold_percent
                )) {
                y++;
                continue;
            }
            start = y;
            while (y < HEIGHT &&
                   is_line_pixel(
                       residue[y * WIDTH + x], line_color, background_color, threshold_percent
                   )) {
                y++;
            }
            end = y - 1;
            if (end - start + 1 >= minimum_length && line_count < MAX_LINES) {
                int clear_y;
                DetectedLine *line = &lines[line_count++];
                line->orientation = "vertical";
                line->start_x = x;
                line->start_y = start;
                line->end_x = x;
                line->end_y = end;
                line->length = end - start + 1;
                line->width = 1;
                line->round = round;
                line->first_pixel_position_from_origin = start * WIDTH + x;
                for (clear_y = start; clear_y <= end; clear_y++) {
                    line->color_values[clear_y - start] = residue[clear_y * WIDTH + x];
                    residue[clear_y * WIDTH + x] = (uint8_t)background_color;
                }
            }
        }
    }
    return line_count;
}

static int detect_horizontal_lines(
    uint8_t residue[PIXEL_COUNT],
    int line_color,
    int background_color,
    DetectedLine lines[MAX_LINES],
    int line_count,
    int minimum_length,
    int threshold_percent,
    int round
) {
    int y;
    for (y = 0; y < HEIGHT; y++) {
        int x = 0;
        while (x < WIDTH) {
            int start;
            int end;
            if (!is_line_pixel(
                    residue[y * WIDTH + x], line_color, background_color, threshold_percent
                )) {
                x++;
                continue;
            }
            start = x;
            while (x < WIDTH &&
                   is_line_pixel(
                       residue[y * WIDTH + x], line_color, background_color, threshold_percent
                   )) {
                x++;
            }
            end = x - 1;
            if (end - start + 1 >= minimum_length && line_count < MAX_LINES) {
                int clear_x;
                DetectedLine *line = &lines[line_count++];
                line->orientation = "horizontal";
                line->start_x = start;
                line->start_y = y;
                line->end_x = end;
                line->end_y = y;
                line->length = end - start + 1;
                line->width = 1;
                line->round = round;
                line->first_pixel_position_from_origin = y * WIDTH + start;
                for (clear_x = start; clear_x <= end; clear_x++) {
                    line->color_values[clear_x - start] = residue[y * WIDTH + clear_x];
                    residue[y * WIDTH + clear_x] = (uint8_t)background_color;
                }
            }
        }
    }
    return line_count;
}

static int detect_dots(
    uint8_t residue[PIXEL_COUNT],
    int line_color,
    int background_color,
    DetectedLine lines[MAX_LINES],
    int line_count
) {
    int y;
    for (y = 0; y < HEIGHT; y++) {
        int x;
        for (x = 0; x < WIDTH; x++) {
            const int index = y * WIDTH + x;
            DetectedLine *dot;
            if (!is_line_pixel(residue[index], line_color, background_color, 6) ||
                line_count >= MAX_LINES) {
                continue;
            }
            dot = &lines[line_count++];
            dot->orientation = "dot";
            dot->start_x = x;
            dot->start_y = y;
            dot->end_x = x;
            dot->end_y = y;
            dot->length = 1;
            dot->width = 1;
            dot->round = 4;
            dot->first_pixel_position_from_origin = index;
            dot->color_values[0] = residue[index];
            residue[index] = (uint8_t)background_color;
        }
    }
    return line_count;
}

static int detect_all_lines(
    const uint8_t original[PIXEL_COUNT],
    uint8_t residue[PIXEL_COUNT],
    int line_color,
    int background_color,
    DetectedLine lines[MAX_LINES],
    int horizontal_first
) {
    int line_count = 0;
    int round;
    const int minimum_lengths[3] = {4, 3, 2};
    memcpy(residue, original, PIXEL_COUNT);
    for (round = 0; round < 3; round++) {
        if (horizontal_first) {
            line_count = detect_horizontal_lines(
                residue, line_color, background_color, lines, line_count,
                minimum_lengths[round], 0, round + 1
            );
            line_count = detect_vertical_lines(
                residue, line_color, background_color, lines, line_count,
                minimum_lengths[round], 0, round + 1
            );
        } else {
            line_count = detect_vertical_lines(
                residue, line_color, background_color, lines, line_count,
                minimum_lengths[round], 0, round + 1
            );
            line_count = detect_horizontal_lines(
                residue, line_color, background_color, lines, line_count,
                minimum_lengths[round], 0, round + 1
            );
        }
    }
    return line_count;
}

static int lines_buffer_symbol_count(const DetectedLine lines[MAX_LINES], int line_count) {
    int group_start = 0;
    int count = 0;
    while (group_start < line_count) {
        const DetectedLine *first = &lines[group_start];
        const int minimum_length = minimum_length_for_round(first->round);
        int group_end = group_start + 1;
        int has_extra_length = first->length != minimum_length;
        int index;
        while (group_end < line_count &&
               lines[group_end].round == first->round &&
               strcmp(lines[group_end].orientation, first->orientation) == 0) {
            if (lines[group_end].length != minimum_length) has_extra_length = 1;
            group_end++;
        }
        count += 3;
        for (index = group_start; index < group_end; index++) {
            count += 2 + lines[index].length + (has_extra_length ? 1 : 0);
        }
        group_start = group_end;
    }
    return count;
}

static int filter_residue_noise(
    uint8_t residue[PIXEL_COUNT],
    int background_color,
    int line_color
) {
    uint8_t source[PIXEL_COUNT];
    int removed = 0;
    const int minimum_color = background_color < line_color ? background_color : line_color;
    const int maximum_color = background_color > line_color ? background_color : line_color;
    int index;
    memcpy(source, residue, PIXEL_COUNT);
    for (index = 0; index < PIXEL_COUNT; index++) {
        const int value = source[index];
        const int difference = abs(background_color - value);
        if (difference <= 6) {
            residue[index] = (uint8_t)background_color;
            if (value != background_color) removed++;
        } else if (difference <= 32 &&
                   value >= minimum_color && value <= maximum_color) {
            const int x = index % WIDTH;
            const int y = index / WIDTH;
            int matching_neighbors = 0;
            if (x > 0 && abs(value - source[index - 1]) <= 4) matching_neighbors++;
            if (x + 1 < WIDTH && abs(value - source[index + 1]) <= 4) matching_neighbors++;
            if (y > 0 && abs(value - source[index - WIDTH]) <= 4) matching_neighbors++;
            if (y + 1 < HEIGHT && abs(value - source[index + WIDTH]) <= 4) matching_neighbors++;
            if (matching_neighbors < 2) {
                residue[index] = (uint8_t)background_color;
                removed++;
            }
        }
    }
    return removed;
}

static int build_output_paths(
    const wchar_t *input_path,
    const wchar_t *output_root,
    wchar_t output_directory[MAX_PATH],
    wchar_t json_path[MAX_PATH],
    wchar_t residue_path[MAX_PATH],
    wchar_t prefilter_residue_path[MAX_PATH],
    wchar_t residue_lines_buffer_path[MAX_PATH],
    wchar_t original_path[MAX_PATH],
    wchar_t original_spectrography_path[MAX_PATH],
    wchar_t original_histogram_path[MAX_PATH],
    wchar_t prefilter_residue_spectrography_path[MAX_PATH],
    wchar_t prefilter_residue_histogram_path[MAX_PATH],
    wchar_t residue_spectrography_path[MAX_PATH],
    wchar_t residue_histogram_path[MAX_PATH]
) {
    const wchar_t *filename = wcsrchr(input_path, L'\\');
    wchar_t block_name[MAX_PATH];
    wchar_t *extension;
    filename = filename == NULL ? input_path : filename + 1;
    if (wcsncpy_s(block_name, MAX_PATH, filename, _TRUNCATE) != 0) return 0;
    extension = wcsrchr(block_name, L'.');
    if (extension) *extension = L'\0';
    CreateDirectoryW(output_root, NULL);
    if (swprintf_s(output_directory, MAX_PATH, L"%ls\\%ls", output_root, block_name) < 0) return 0;
    if (!CreateDirectoryW(output_directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (swprintf_s(json_path, MAX_PATH, L"%ls\\horizontal_vertical_lines.json", output_directory) < 0) return 0;
    if (swprintf_s(residue_path, MAX_PATH, L"%ls\\residue.png", output_directory) < 0) return 0;
    if (swprintf_s(prefilter_residue_path, MAX_PATH, L"%ls\\prefilter_residue.png", output_directory) < 0) return 0;
    if (swprintf_s(residue_lines_buffer_path, MAX_PATH, L"%ls\\residue_linesBuffer.png", output_directory) < 0) return 0;
    if (swprintf_s(original_path, MAX_PATH, L"%ls\\original.png", output_directory) < 0) return 0;
    if (swprintf_s(original_spectrography_path, MAX_PATH, L"%ls\\original_spectrography.json", output_directory) < 0) return 0;
    if (swprintf_s(original_histogram_path, MAX_PATH, L"%ls\\original_band_histogram.png", output_directory) < 0) return 0;
    if (swprintf_s(prefilter_residue_spectrography_path, MAX_PATH, L"%ls\\prefilter_residue_spectrography.json", output_directory) < 0) return 0;
    if (swprintf_s(prefilter_residue_histogram_path, MAX_PATH, L"%ls\\prefilter_residue_band_histogram.png", output_directory) < 0) return 0;
    if (swprintf_s(residue_spectrography_path, MAX_PATH, L"%ls\\residue_spectrography.json", output_directory) < 0) return 0;
    if (swprintf_s(residue_histogram_path, MAX_PATH, L"%ls\\residue_band_histogram.png", output_directory) < 0) return 0;
    return 1;
}

static int write_spectrography_json(
    const wchar_t *output_path,
    const SpectralBand bands[BAND_COUNT],
    double energy_percent,
    double energy_concentration,
    const char *banding_method
) {
    FILE *file = NULL;
    int i;
    if (_wfopen_s(&file, output_path, L"wb") != 0 || !file) return 0;
    fprintf(file, "{\n");
    fprintf(file, "  \"version\": 2,\n");
    fprintf(file, "  \"channel\": \"Y\",\n");
    fprintf(file, "  \"width\": 16,\n");
    fprintf(file, "  \"height\": 16,\n");
    fprintf(file, "  \"pixel_count\": 256,\n");
    fprintf(file, "  \"band_count\": 16,\n");
    fprintf(file, "  \"banding_method\": \"%s\",\n", banding_method);
    fprintf(file, "  \"energy\": %.6f,\n", energy_percent);
    fprintf(file, "  \"energy_concentration\": %.10f,\n", energy_concentration);
    fprintf(file, "  \"energy_concentration_percent\": %.6f,\n", energy_concentration * 100.0);
    fprintf(file, "  \"bands\": [\n");
    for (i = 0; i < BAND_COUNT; i++) {
        fprintf(
            file,
            "    {\"band\": %d, \"range\": [%d, %d], \"width\": %d, \"count\": %d, "
            "\"weight_percent\": %.6f, \"peak_value\": ",
            i, bands[i].min_value, bands[i].max_value, bands[i].width,
            bands[i].count, bands[i].weight_percent
        );
        if (bands[i].count > 0) fprintf(file, "%.6f", bands[i].peak_value);
        else fprintf(file, "null");
        fprintf(file, "}%s\n", i + 1 == BAND_COUNT ? "" : ",");
    }
    fprintf(file, "  ]\n}\n");
    fclose(file);
    return 1;
}

static void write_candidate_lines(
    FILE *file,
    const char *key,
    const DetectedLine lines[MAX_LINES],
    int line_count,
    int line_color,
    int background_color
) {
    int index;
    fprintf(file, "  \"%s\": [\n", key);
    for (index = 0; index < line_count; index++) {
        const DetectedLine *line = &lines[index];
        int color_index;
        fprintf(
            file,
            "    {\"lineId\": %d, \"orientation\": \"%s\", \"round\": %d, "
            "\"start\": {\"x\": %d, \"y\": %d}, \"end\": {\"x\": %d, \"y\": %d}, "
            "\"length\": %d, \"colorValues4bit\": [",
            index, line->orientation, line->round, line->start_x, line->start_y,
            line->end_x, line->end_y, line->length
        );
        for (color_index = 0; color_index < line->length; color_index++) {
            fprintf(
                file, "%d%s",
                color_to_4bit(line->color_values[color_index], line_color, background_color),
                color_index + 1 == line->length ? "" : ", "
            );
        }
        fprintf(file, "]}%s\n", index + 1 == line_count ? "" : ",");
    }
    fprintf(file, "  ],\n");
}

static void write_candidate_buffer(
    FILE *file,
    const char *key,
    const DetectedLine lines[MAX_LINES],
    int line_count,
    int line_color,
    int background_color
) {
    int group_start = 0;
    int first_symbol = 1;
    fprintf(file, "  \"%s\": [", key);
#define WRITE_CANDIDATE_SYMBOL(symbol) \
    do { \
        fprintf(file, "%s%d", first_symbol ? "" : ", ", (symbol)); \
        first_symbol = 0; \
    } while (0)
    while (group_start < line_count) {
        const DetectedLine *first = &lines[group_start];
        const int minimum_length = minimum_length_for_round(first->round);
        int group_end = group_start + 1;
        int has_extra_length = first->length != minimum_length;
        int line_index;
        while (group_end < line_count &&
               lines[group_end].round == first->round &&
               strcmp(lines[group_end].orientation, first->orientation) == 0) {
            if (lines[group_end].length != minimum_length) has_extra_length = 1;
            group_end++;
        }
        WRITE_CANDIDATE_SYMBOL(minimum_length);
        WRITE_CANDIDATE_SYMBOL(group_end - group_start);
        WRITE_CANDIDATE_SYMBOL(orientation_type(first->orientation));
        for (line_index = group_start; line_index < group_end; line_index++) {
            const DetectedLine *line = &lines[line_index];
            const DetectedLine *previous = line_index > group_start ? &lines[line_index - 1] : NULL;
            int previous_color;
            int color_index;
            if (has_extra_length) WRITE_CANDIDATE_SYMBOL(line->length - minimum_length);
            WRITE_CANDIDATE_SYMBOL(previous ? abs(line->start_x - previous->start_x) : line->start_x);
            WRITE_CANDIDATE_SYMBOL(previous ? abs(line->start_y - previous->start_y) : line->start_y);
            previous_color = color_to_4bit(line->color_values[0], line_color, background_color);
            WRITE_CANDIDATE_SYMBOL(previous_color);
            for (color_index = 1; color_index < line->length; color_index++) {
                int current_color = color_to_4bit(
                    line->color_values[color_index], line_color, background_color
                );
                WRITE_CANDIDATE_SYMBOL(zigzag_encode_delta(current_color - previous_color));
                previous_color = current_color;
            }
        }
        group_start = group_end;
    }
#undef WRITE_CANDIDATE_SYMBOL
    fprintf(file, "],\n");
}

static int write_json(
    const wchar_t *output_path,
    int line_color,
    int background_color,
    int line_band,
    int background_band,
    double energy_percent,
    double energy_concentration,
    int encoder_applied,
    const SpectralBand bands[BAND_COUNT],
    int noise_filter_enabled,
    int noise_pixels_removed,
    int noise_pass_1_removed,
    int noise_pass_2_removed,
    const DetectedLine lines_v[MAX_LINES],
    int line_count_v,
    int buffer_symbols_v,
    const DetectedLine lines_h[MAX_LINES],
    int line_count_h,
    int buffer_symbols_h,
    const char *selected_order,
    const DetectedLine lines[MAX_LINES],
    int line_count,
    const uint8_t residue[PIXEL_COUNT]
) {
    FILE *file = NULL;
    int index;
    int row;
    if (_wfopen_s(&file, output_path, L"wb") != 0 || file == NULL) return 0;
    fprintf(file, "{\n");
    fprintf(file, "  \"width\": 16,\n");
    fprintf(file, "  \"height\": 16,\n");
    fprintf(file, "  \"lineColor\": %d,\n", line_color);
    fprintf(file, "  \"lineColorBits\": %d,\n", g_line_color_bits);
    fprintf(file, "  \"lineColorMaxCode\": %d,\n", line_color_max_code());
    fprintf(file, "  \"backgroundColor\": %d,\n", background_color);
    fprintf(file, "  \"lineBand\": %d,\n", line_band);
    fprintf(file, "  \"backgroundBand\": %d,\n", background_band);
    fprintf(file, "  \"energy\": %.6f,\n", energy_percent);
    fprintf(file, "  \"energyConcentration\": %.10f,\n", energy_concentration);
    fprintf(file, "  \"energyConcentrationPercent\": %.6f,\n", energy_concentration * 100.0);
    fprintf(file, "  \"encoderApplied\": %s,\n", encoder_applied ? "true" : "false");
    fprintf(file, "  \"gate\": {\"minimumEnergy\": 35.0, \"minimumEnergyConcentration\": 0.4},\n");
    fprintf(file, "  \"spectralBands\": [\n");
    for (index = 0; index < BAND_COUNT; index++) {
        fprintf(
            file,
            "    {\"band\": %d, \"range\": [%d, %d], \"count\": %d, "
            "\"weightPercent\": %.6f, \"peakValue\": ",
            index, bands[index].min_value, bands[index].max_value,
            bands[index].count, bands[index].weight_percent
        );
        if (bands[index].count > 0) fprintf(file, "%.6f", bands[index].peak_value);
        else fprintf(file, "null");
        fprintf(file, "}%s\n", index + 1 == BAND_COUNT ? "" : ",");
    }
    fprintf(file, "  ],\n");
    fprintf(file, "  \"noiseFilter\": {\n");
    fprintf(file, "    \"enabled\": %s,\n", noise_filter_enabled ? "true" : "false");
    fprintf(file, "    \"backgroundDifferenceThreshold\": 6,\n");
    fprintf(file, "    \"neighborCheckBackgroundDifferenceThreshold\": 32,\n");
    fprintf(file, "    \"neighborMode\": \"left-right-top-bottom\",\n");
    fprintf(file, "    \"neighborValueThreshold\": 4,\n");
    fprintf(file, "    \"minimumMatchingNeighborsToKeep\": 2,\n");
    fprintf(file, "    \"passes\": 1,\n");
    fprintf(file, "    \"pass1PixelsRemoved\": %d,\n", noise_pass_1_removed);
    fprintf(file, "    \"pass2PixelsRemoved\": %d,\n", noise_pass_2_removed);
    fprintf(file, "    \"pixelsRemoved\": %d\n", noise_pixels_removed);
    fprintf(file, "  },\n");
    fprintf(file, "  \"rounds\": [\n");
    fprintf(file, "    {\"round\": 1, \"minimumLineLength\": 4},\n");
    fprintf(file, "    {\"round\": 2, \"minimumLineLength\": 3},\n");
    fprintf(file, "    {\"round\": 3, \"minimumLineLength\": 2}\n");
    fprintf(file, "  ],\n");
    fprintf(file, "  \"lineWidth\": 1,\n");
    fprintf(file, "  \"selectedOrder\": \"%s\",\n", selected_order);
    fprintf(
        file,
        "  \"candidateSummary\": {\"verticalFirst\": {\"lineCount\": %d, "
        "\"bufferSymbols\": %d}, \"horizontalFirst\": {\"lineCount\": %d, "
        "\"bufferSymbols\": %d}},\n",
        line_count_v, buffer_symbols_v, line_count_h, buffer_symbols_h
    );
    write_candidate_lines(file, "lines_v", lines_v, line_count_v, line_color, background_color);
    write_candidate_buffer(file, "linesBuffer_v", lines_v, line_count_v, line_color, background_color);
    write_candidate_lines(file, "lines_h", lines_h, line_count_h, line_color, background_color);
    write_candidate_buffer(file, "linesBuffer_h", lines_h, line_count_h, line_color, background_color);
    fprintf(file, "  \"lineCount\": %d,\n", line_count);
    fprintf(file, "  \"lines\": [\n");
    for (index = 0; index < line_count; index++) {
        const DetectedLine *line = &lines[index];
        const DetectedLine *previous = index > 0 ? &lines[index - 1] : NULL;
        int same_group = 0;
        int relative_x = 0;
        int relative_y = 0;
        int color_index;
        if (previous != NULL &&
            previous->round == line->round &&
            strcmp(previous->orientation, line->orientation) == 0) {
            same_group = 1;
            relative_x = abs(line->start_x - previous->start_x);
            relative_y = abs(line->start_y - previous->start_y);
        }
        fprintf(
            file,
            "    {\"lineId\": %d, \"orientation\": \"%s\", "
            "\"start\": {\"x\": %d, \"y\": %d}, "
            "\"end\": {\"x\": %d, \"y\": %d}, "
            "\"length\": %d, \"width\": %d, \"round\": %d, "
            "\"firstPixelPositionFromOrigin\": {\"x\": %d, \"y\": %d}, "
            "\"positionRelative\": {\"x\": %d, \"y\": %d}, "
            "\"colorValues\": [",
            index, line->orientation,
            line->start_x, line->start_y,
            line->end_x, line->end_y,
            line->length, line->width, line->round,
            line->start_x, line->start_y, relative_x, relative_y
        );
        for (color_index = 0; color_index < line->length; color_index++) {
            fprintf(
                file, "%u%s", line->color_values[color_index],
                color_index + 1 == line->length ? "" : ", "
            );
        }
        fprintf(file, "], \"colorValues4bit\": [");
        for (color_index = 0; color_index < line->length; color_index++) {
            fprintf(
                file, "%d%s",
                color_to_4bit(line->color_values[color_index], line_color, background_color),
                color_index + 1 == line->length ? "" : ", "
            );
        }
        fprintf(
            file, "], \"encodedSymbols\": [%d, %d",
            same_group ? relative_x : line->start_x,
            same_group ? relative_y : line->start_y
        );
        if (line->length > 0) {
            int previous_color = color_to_4bit(
                line->color_values[0], line_color, background_color
            );
            fprintf(file, ", %d", previous_color);
            for (color_index = 1; color_index < line->length; color_index++) {
                const int current_color = color_to_4bit(
                    line->color_values[color_index], line_color, background_color
                );
                fprintf(file, ", %d", zigzag_encode_delta(current_color - previous_color));
                previous_color = current_color;
            }
        }
        fprintf(file, "]}%s\n", index + 1 == line_count ? "" : ",");
    }
    fprintf(file, "  ],\n");
    fprintf(file, "  \"linesBuffer\": [");
    {
        int group_start = 0;
        int first_block_symbol = 1;
        while (group_start < line_count) {
            const DetectedLine *first = &lines[group_start];
            const int minimum_length = minimum_length_for_round(first->round);
            const int type = orientation_type(first->orientation);
            int group_end = group_start + 1;
            int has_extra_length = first->length != minimum_length;
            int line_index;
            int first_symbol = first_block_symbol;
            while (group_end < line_count &&
                   lines[group_end].round == first->round &&
                   strcmp(lines[group_end].orientation, first->orientation) == 0) {
                if (lines[group_end].length != minimum_length) has_extra_length = 1;
                group_end++;
            }
#define WRITE_BLOCK_SYMBOL(symbol) \
            do { \
                fprintf(file, "%s%d", first_symbol ? "" : ", ", (symbol)); \
                first_symbol = 0; \
                first_block_symbol = 0; \
            } while (0)
            WRITE_BLOCK_SYMBOL(minimum_length);
            WRITE_BLOCK_SYMBOL(group_end - group_start);
            WRITE_BLOCK_SYMBOL(type);
            for (line_index = group_start; line_index < group_end; line_index++) {
                const DetectedLine *line = &lines[line_index];
                const DetectedLine *previous = line_index > group_start
                    ? &lines[line_index - 1]
                    : NULL;
                const int position_x = previous == NULL
                    ? line->start_x
                    : abs(line->start_x - previous->start_x);
                const int position_y = previous == NULL
                    ? line->start_y
                    : abs(line->start_y - previous->start_y);
                int previous_color;
                int color_index;
                if (has_extra_length) WRITE_BLOCK_SYMBOL(line->length - minimum_length);
                WRITE_BLOCK_SYMBOL(position_x);
                WRITE_BLOCK_SYMBOL(position_y);
                previous_color = color_to_4bit(
                    line->color_values[0], line_color, background_color
                );
                WRITE_BLOCK_SYMBOL(previous_color);
                for (color_index = 1; color_index < line->length; color_index++) {
                    const int current_color = color_to_4bit(
                        line->color_values[color_index], line_color, background_color
                    );
                    WRITE_BLOCK_SYMBOL(zigzag_encode_delta(current_color - previous_color));
                    previous_color = current_color;
                }
            }
            group_start = group_end;
#undef WRITE_BLOCK_SYMBOL
        }
    }
    fprintf(file, "],\n");
    fprintf(file, "  \"residueY\": {\n");
    for (row = 0; row < HEIGHT; row++) {
        int column;
        fprintf(file, "    \"row_%d\": [", row);
        for (column = 0; column < WIDTH; column++) {
            fprintf(
                file,
                "%u%s",
                residue[row * WIDTH + column],
                column + 1 == WIDTH ? "" : ", "
            );
        }
        fprintf(file, "]%s\n", row + 1 == HEIGHT ? "" : ",");
    }
    fprintf(file, "  }\n");
    fprintf(file, "}\n");
    fclose(file);
    return 1;
}

int wmain(int argc, wchar_t **argv) {
    uint8_t original[PIXEL_COUNT];
    uint8_t residue[PIXEL_COUNT];
    uint8_t residue_v[PIXEL_COUNT];
    uint8_t residue_h[PIXEL_COUNT];
    uint8_t prefilter_residue[PIXEL_COUNT];
    uint8_t residue_lines_buffer[PIXEL_COUNT];
    DetectedLine lines[MAX_LINES];
    DetectedLine lines_v[MAX_LINES];
    DetectedLine lines_h[MAX_LINES];
    SpectralBand bands[BAND_COUNT];
    SpectralBand residue_bands[BAND_COUNT];
    SpectralBand prefilter_residue_bands[BAND_COUNT];
    int line_color;
    int background_color;
    int line_band;
    int background_band;
    double energy_percent;
    double energy_concentration;
    double residue_energy_percent;
    double residue_energy_concentration;
    double prefilter_residue_energy_percent;
    double prefilter_residue_energy_concentration;
    int encoder_applied;
    int automatic_noise_filter_applied = 0;
    int line_count = 0;
    int line_count_v = 0;
    int line_count_h = 0;
    int buffer_symbols_v = 0;
    int buffer_symbols_h = 0;
    const char *selected_order = "vertical-first";
    int noise_filter_enabled = 0;
    int noise_pixels_removed = 0;
    int noise_pass_1_removed = 0;
    int noise_pass_2_removed = 0;
    HRESULT result;
    wchar_t output_directory[MAX_PATH];
    wchar_t json_path[MAX_PATH];
    wchar_t residue_path[MAX_PATH];
    wchar_t prefilter_residue_path[MAX_PATH];
    wchar_t residue_lines_buffer_path[MAX_PATH];
    wchar_t original_path[MAX_PATH];
    wchar_t original_spectrography_path[MAX_PATH];
    wchar_t original_histogram_path[MAX_PATH];
    wchar_t prefilter_residue_spectrography_path[MAX_PATH];
    wchar_t prefilter_residue_histogram_path[MAX_PATH];
    wchar_t residue_spectrography_path[MAX_PATH];
    wchar_t residue_histogram_path[MAX_PATH];

    if (argc < 3 || argc > 5) {
        fwprintf(
            stderr,
            L"Usage: %ls input.png output-root [noise-filter=0] [line-color-bits=4]\n",
            argv[0]
        );
        return 1;
    }
    if (argc >= 4) noise_filter_enabled = RESIDUE_FILTER_ENABLED && _wtoi(argv[3]) != 0;
    if (argc >= 5) {
        g_line_color_bits = _wtoi(argv[4]);
        if (g_line_color_bits != 6 && g_line_color_bits != 4 && g_line_color_bits != 2) {
            fwprintf(stderr, L"line-color-bits must be 6, 4, or 2\n");
            return 1;
        }
    }
    if (!build_output_paths(
            argv[1], argv[2], output_directory, json_path, residue_path,
            prefilter_residue_path, residue_lines_buffer_path, original_path,
            original_spectrography_path, original_histogram_path,
            prefilter_residue_spectrography_path, prefilter_residue_histogram_path,
            residue_spectrography_path, residue_histogram_path
        )) {
        return 1;
    }
    result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(result)) return 1;
    if (!load_png_y_channel(argv[1], original)) {
        CoUninitialize();
        return 1;
    }
    if (!CopyFileW(argv[1], original_path, FALSE)) {
        fwprintf(stderr, L"Failed to copy original PNG to: %ls\n", original_path);
        CoUninitialize();
        return 1;
    }
    memcpy(residue, original, sizeof(residue));
    analyze_spectrum(original, bands, &energy_percent, &energy_concentration);
    select_colors(
        bands, &background_band, &line_band, &background_color, &line_color
    );
    encoder_applied = energy_percent >= 35.0 && energy_concentration >= 0.4;
    if (encoder_applied) {
        line_count_v = detect_all_lines(
            original, residue_v, line_color, background_color, lines_v, 0
        );
        line_count_h = detect_all_lines(
            original, residue_h, line_color, background_color, lines_h, 1
        );
        buffer_symbols_v = lines_buffer_symbol_count(lines_v, line_count_v);
        buffer_symbols_h = lines_buffer_symbol_count(lines_h, line_count_h);
        if (buffer_symbols_h < buffer_symbols_v ||
            (buffer_symbols_h == buffer_symbols_v && line_count_h < line_count_v)) {
            selected_order = "horizontal-first";
            line_count = line_count_h;
            memcpy(lines, lines_h, sizeof(lines));
            memcpy(residue, residue_h, sizeof(residue));
        } else {
            line_count = line_count_v;
            memcpy(lines, lines_v, sizeof(lines));
            memcpy(residue, residue_v, sizeof(residue));
        }
    }
    analyze_adaptive_spectrum(
        residue, prefilter_residue_bands,
        &prefilter_residue_energy_percent, &prefilter_residue_energy_concentration
    );
    memcpy(prefilter_residue, residue, sizeof(prefilter_residue));
    automatic_noise_filter_applied =
        RESIDUE_FILTER_ENABLED && encoder_applied &&
        prefilter_residue_energy_percent < 25.0 &&
        prefilter_residue_energy_concentration > 0.5;
    if (encoder_applied && (noise_filter_enabled || automatic_noise_filter_applied)) {
        noise_pass_1_removed = filter_residue_noise(residue, background_color, line_color);
        noise_pixels_removed = noise_pass_1_removed;
    }
    analyze_adaptive_spectrum(
        residue, residue_bands, &residue_energy_percent, &residue_energy_concentration
    );
    redraw_lines_buffer(
        residue_lines_buffer, residue, lines, line_count, line_color, background_color
    );
    if (!write_json(
            json_path, line_color, background_color, line_band, background_band,
            energy_percent, energy_concentration, encoder_applied, bands,
            noise_filter_enabled || automatic_noise_filter_applied, noise_pixels_removed,
            noise_pass_1_removed, noise_pass_2_removed,
            lines_v, line_count_v, buffer_symbols_v,
            lines_h, line_count_h, buffer_symbols_h, selected_order,
            lines, line_count, residue
        ) ||
        !write_y_plane_png(prefilter_residue_path, prefilter_residue) ||
        !write_y_plane_png(residue_path, residue) ||
        !write_y_plane_png(residue_lines_buffer_path, residue_lines_buffer) ||
        !write_spectrography_json(
            original_spectrography_path, bands, energy_percent, energy_concentration,
            "fixed_16_value_bands"
        ) ||
        !write_band_histogram_png(original_histogram_path, bands) ||
        !write_spectrography_json(
            prefilter_residue_spectrography_path, prefilter_residue_bands,
            prefilter_residue_energy_percent, prefilter_residue_energy_concentration,
            "adaptive_energy_region"
        ) ||
        !write_band_histogram_png(
            prefilter_residue_histogram_path, prefilter_residue_bands
        ) ||
        !write_spectrography_json(
            residue_spectrography_path, residue_bands,
            residue_energy_percent, residue_energy_concentration,
            "adaptive_energy_region"
        ) ||
        !write_band_histogram_png(residue_histogram_path, residue_bands)) {
        CoUninitialize();
        return 1;
    }
    wprintf(L"Output directory: %ls\n", output_directory);
    wprintf(L"Lines JSON: %ls\n", json_path);
    wprintf(L"Residue PNG: %ls\n", residue_path);
    wprintf(L"Prefilter residue PNG: %ls\n", prefilter_residue_path);
    wprintf(L"Residue linesBuffer PNG: %ls\n", residue_lines_buffer_path);
    wprintf(L"Original PNG: %ls\n", original_path);
    wprintf(L"Original spectrography JSON: %ls\n", original_spectrography_path);
    wprintf(L"Original band histogram: %ls\n", original_histogram_path);
    wprintf(L"Prefilter residue spectrography JSON: %ls\n", prefilter_residue_spectrography_path);
    wprintf(L"Prefilter residue band histogram: %ls\n", prefilter_residue_histogram_path);
    wprintf(L"Residue spectrography JSON: %ls\n", residue_spectrography_path);
    wprintf(L"Residue band histogram: %ls\n", residue_histogram_path);
    printf("Lines found: %d\n", line_count);
    printf("Selected order: %s\n", selected_order);
    printf("Vertical-first: %d lines, %d symbols\n", line_count_v, buffer_symbols_v);
    printf("Horizontal-first: %d lines, %d symbols\n", line_count_h, buffer_symbols_h);
    printf("Energy: %.6f%%\n", energy_percent);
    printf("Energy concentration: %.6f%%\n", energy_concentration * 100.0);
    printf("Background: band %d, color %d\n", background_band, background_color);
    printf("Line: band %d, color %d\n", line_band, line_color);
    printf("Line color bits: %d (max code %d)\n", g_line_color_bits, line_color_max_code());
    printf("Encoder applied: %s\n", encoder_applied ? "yes" : "no");
    printf("Residue energy: %.6f%%\n", residue_energy_percent);
    printf("Residue energy concentration: %.6f%%\n", residue_energy_concentration * 100.0);
    printf("Noise filter: %s\n", noise_filter_enabled ? "on" : "off");
    printf("Automatic residue noise filter: %s\n", automatic_noise_filter_applied ? "applied" : "not applied");
    printf("Noise pixels removed: %d\n", noise_pixels_removed);
    printf("Noise pass 1 removed: %d\n", noise_pass_1_removed);
    printf("Noise pass 2 removed: %d\n", noise_pass_2_removed);
    CoUninitialize();
    return 0;
}

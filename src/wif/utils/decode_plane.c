#include "wif/utils/decode_plane.h"

#include "wif/utils/rans_route_map.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIF_DECODE_BLOCK_SIZE 16u

static void set_error(char *buffer, size_t buffer_size, const char *message)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", message);
}

static uint8_t get_bit_msb_first(const uint8_t *bytes, uint32_t bit_index)
{
    return (uint8_t)((bytes[bit_index / 8u] >> (7u - (bit_index % 8u))) & 1u);
}

static int has_png_extension(const char *path)
{
    size_t length = strlen(path);

    if (length < 4) {
        return 0;
    }
    return _stricmp(path + length - 4, ".png") == 0;
}

static wchar_t *utf8_to_wide(const char *text)
{
    wchar_t *wide;
    int count;

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

static int read_raw_plane(const char *path, uint8_t *pixels, size_t pixel_count)
{
    FILE *file = fopen(path, "rb");
    long size;

    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    size = ftell(file);
    if (size < 0 || (size_t)size != pixel_count) {
        fclose(file);
        return 0;
    }
    rewind(file);
    if (pixel_count > 0 && fread(pixels, 1, pixel_count, file) != pixel_count) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static int write_raw_plane(const char *path, const uint8_t *pixels, size_t pixel_count)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return 0;
    }
    if (pixel_count > 0 && fwrite(pixels, 1, pixel_count, file) != pixel_count) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static int read_png_plane(
    IWICImagingFactory *factory,
    const char *path,
    uint32_t expected_width,
    uint32_t expected_height,
    uint8_t *pixels,
    char *error_message,
    size_t error_message_size
)
{
    wchar_t *wide_path = utf8_to_wide(path);
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    UINT width = 0;
    UINT height = 0;
    HRESULT hr;
    int ok = 0;

    if (wide_path == NULL) {
        set_error(error_message, error_message_size, "failed to convert PNG path");
        return 0;
    }

    hr = IWICImagingFactory_CreateDecoderFromFilename(factory, wide_path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to decode PNG plane");
        goto cleanup;
    }
    hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to read PNG frame");
        goto cleanup;
    }
    hr = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    if (FAILED(hr) || width != expected_width || height != expected_height) {
        set_error(error_message, error_message_size, "PNG plane dimensions do not match arguments");
        goto cleanup;
    }
    hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create PNG format converter");
        goto cleanup;
    }
    hr = IWICFormatConverter_Initialize(
        converter,
        (IWICBitmapSource *)frame,
        &GUID_WICPixelFormat8bppGray,
        WICBitmapDitherTypeNone,
        NULL,
        0.0,
        WICBitmapPaletteTypeCustom
    );
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to convert PNG plane to grayscale");
        goto cleanup;
    }
    hr = IWICFormatConverter_CopyPixels(converter, NULL, expected_width, (UINT)((size_t)expected_width * expected_height), pixels);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to copy PNG plane pixels");
        goto cleanup;
    }

    ok = 1;

cleanup:
    if (converter != NULL) {
        IWICFormatConverter_Release(converter);
    }
    if (frame != NULL) {
        IWICBitmapFrameDecode_Release(frame);
    }
    if (decoder != NULL) {
        IWICBitmapDecoder_Release(decoder);
    }
    free(wide_path);
    return ok;
}

static int write_gray_png(
    IWICImagingFactory *factory,
    const char *path,
    uint32_t width,
    uint32_t height,
    const uint8_t *pixels,
    char *error_message,
    size_t error_message_size
)
{
    wchar_t *wide_path = utf8_to_wide(path);
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *property_bag = NULL;
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat8bppGray;
    HRESULT hr;
    int ok = 0;

    if (wide_path == NULL) {
        set_error(error_message, error_message_size, "failed to convert output PNG path");
        return 0;
    }
    hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create PNG stream");
        goto cleanup;
    }
    hr = IWICStream_InitializeFromFilename(stream, wide_path, GENERIC_WRITE);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to open output PNG");
        goto cleanup;
    }
    hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create PNG encoder");
        goto cleanup;
    }
    hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to initialize PNG encoder");
        goto cleanup;
    }
    hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &property_bag);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create PNG frame");
        goto cleanup;
    }
    hr = IWICBitmapFrameEncode_Initialize(frame, property_bag);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to initialize PNG frame");
        goto cleanup;
    }
    hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to set PNG size");
        goto cleanup;
    }
    hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &pixel_format);
    if (FAILED(hr) || !IsEqualGUID(&pixel_format, &GUID_WICPixelFormat8bppGray)) {
        set_error(error_message, error_message_size, "failed to set output PNG format");
        goto cleanup;
    }
    hr = IWICBitmapFrameEncode_WritePixels(frame, height, width, width * height, (BYTE *)pixels);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to write output PNG pixels");
        goto cleanup;
    }
    hr = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to commit output PNG frame");
        goto cleanup;
    }
    hr = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to commit output PNG");
        goto cleanup;
    }

    ok = 1;

cleanup:
    if (property_bag != NULL) {
        IPropertyBag2_Release(property_bag);
    }
    if (frame != NULL) {
        IWICBitmapFrameEncode_Release(frame);
    }
    if (encoder != NULL) {
        IWICBitmapEncoder_Release(encoder);
    }
    if (stream != NULL) {
        IWICStream_Release(stream);
    }
    free(wide_path);
    return ok;
}

static int make_directory(const char *path)
{
    wchar_t *wide_path = utf8_to_wide(path);
    int ok;

    if (wide_path == NULL) {
        return 0;
    }
    ok = CreateDirectoryW(wide_path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(wide_path);
    return ok;
}

static int join_path(const char *directory, const char *file_name, char *output, size_t output_size)
{
    size_t directory_length = strlen(directory);
    const char separator =
        directory_length > 0 && (directory[directory_length - 1] == '\\' || directory[directory_length - 1] == '/')
            ? '\0'
            : '\\';
    int written = separator == '\0'
        ? snprintf(output, output_size, "%s%s", directory, file_name)
        : snprintf(output, output_size, "%s%c%s", directory, separator, file_name);

    return written > 0 && (size_t)written < output_size;
}

static int load_plane(
    IWICImagingFactory *factory,
    const char *path,
    uint32_t width,
    uint32_t height,
    uint8_t *pixels,
    char *error_message,
    size_t error_message_size
)
{
    if (has_png_extension(path)) {
        return read_png_plane(factory, path, width, height, pixels, error_message, error_message_size);
    }
    if (!read_raw_plane(path, pixels, (size_t)width * height)) {
        set_error(error_message, error_message_size, "failed to read raw compact plane");
        return 0;
    }
    return 1;
}

int wif_decode_plane(
    const char *route_map_rans_path,
    uint32_t width,
    uint32_t height,
    const char *dtc_plane_path,
    uint32_t dtc_width,
    uint32_t dtc_height,
    const char *wave_plane_path,
    uint32_t wave_width,
    uint32_t wave_height,
    const char *output_dir,
    WifDecodePlaneResult *result,
    char *error_message,
    size_t error_message_size
)
{
    IWICImagingFactory *factory = NULL;
    uint8_t *route_map = NULL;
    uint8_t *dtc_plane = NULL;
    uint8_t *wave_plane = NULL;
    uint8_t *decoded = NULL;
    uint32_t route_bit_count = 0;
    uint32_t route_byte_count = 0;
    uint32_t expected_blocks;
    uint32_t block_index = 0;
    uint32_t dtc_index = 0;
    uint32_t wave_index = 0;
    uint32_t dtc_blocks_per_row;
    uint32_t wave_blocks_per_row;
    char raw_path[MAX_PATH];
    char png_path[MAX_PATH];
    HRESULT hr;
    int initialized_com = 0;
    int ok = 0;

    if (route_map_rans_path == NULL || dtc_plane_path == NULL || wave_plane_path == NULL || output_dir == NULL ||
        result == NULL || width == 0 || height == 0 || dtc_width == 0 || dtc_height == 0 || wave_width == 0 || wave_height == 0) {
        set_error(error_message, error_message_size, "invalid decodePlane argument");
        return 0;
    }

    memset(result, 0, sizeof(*result));
    expected_blocks = ((width + WIF_DECODE_BLOCK_SIZE - 1u) / WIF_DECODE_BLOCK_SIZE) *
        ((height + WIF_DECODE_BLOCK_SIZE - 1u) / WIF_DECODE_BLOCK_SIZE);

    if (!wif_rans_route_map_decode_to_memory(route_map_rans_path, &route_map, &route_bit_count, &route_byte_count, error_message, error_message_size)) {
        goto cleanup;
    }
    if (route_bit_count != expected_blocks) {
        set_error(error_message, error_message_size, "route map block count does not match output dimensions");
        goto cleanup;
    }

    dtc_plane = (uint8_t *)malloc((size_t)dtc_width * dtc_height);
    wave_plane = (uint8_t *)malloc((size_t)wave_width * wave_height);
    decoded = (uint8_t *)calloc((size_t)width * height, 1);
    if (dtc_plane == NULL || wave_plane == NULL || decoded == NULL) {
        set_error(error_message, error_message_size, "failed to allocate decodePlane buffers");
        goto cleanup;
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        initialized_com = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        set_error(error_message, error_message_size, "failed to initialize COM");
        goto cleanup;
    }
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (LPVOID *)&factory);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create WIC factory");
        goto cleanup;
    }

    if (!load_plane(factory, dtc_plane_path, dtc_width, dtc_height, dtc_plane, error_message, error_message_size) ||
        !load_plane(factory, wave_plane_path, wave_width, wave_height, wave_plane, error_message, error_message_size)) {
        goto cleanup;
    }

    dtc_blocks_per_row = dtc_width / WIF_DECODE_BLOCK_SIZE;
    wave_blocks_per_row = wave_width / WIF_DECODE_BLOCK_SIZE;
    if (dtc_blocks_per_row == 0 || wave_blocks_per_row == 0) {
        set_error(error_message, error_message_size, "compact plane dimensions are too small");
        goto cleanup;
    }

    for (uint32_t block_y = 0; block_y < height; block_y += WIF_DECODE_BLOCK_SIZE) {
        uint32_t block_height = height - block_y < WIF_DECODE_BLOCK_SIZE ? height - block_y : WIF_DECODE_BLOCK_SIZE;

        for (uint32_t block_x = 0; block_x < width; block_x += WIF_DECODE_BLOCK_SIZE) {
            uint32_t block_width = width - block_x < WIF_DECODE_BLOCK_SIZE ? width - block_x : WIF_DECODE_BLOCK_SIZE;
            uint8_t is_wave = get_bit_msb_first(route_map, block_index++);
            const uint8_t *source;
            uint32_t source_width;
            uint32_t packed_index;
            uint32_t packed_block_x;
            uint32_t packed_block_y;

            if (is_wave) {
                source = wave_plane;
                source_width = wave_width;
                packed_index = wave_index++;
                packed_block_x = (packed_index % wave_blocks_per_row) * WIF_DECODE_BLOCK_SIZE;
                packed_block_y = (packed_index / wave_blocks_per_row) * WIF_DECODE_BLOCK_SIZE;
            } else {
                source = dtc_plane;
                source_width = dtc_width;
                packed_index = dtc_index++;
                packed_block_x = (packed_index % dtc_blocks_per_row) * WIF_DECODE_BLOCK_SIZE;
                packed_block_y = (packed_index / dtc_blocks_per_row) * WIF_DECODE_BLOCK_SIZE;
            }

            for (uint32_t y = 0; y < block_height; ++y) {
                for (uint32_t x = 0; x < block_width; ++x) {
                    size_t src_index = (size_t)(packed_block_y + y) * source_width + (packed_block_x + x);
                    size_t dst_index = (size_t)(block_y + y) * width + (block_x + x);
                    decoded[dst_index] = source[src_index];
                }
            }
        }
    }

    if (!make_directory(output_dir) ||
        !join_path(output_dir, "decoded_y_plane.raw", raw_path, sizeof(raw_path)) ||
        !join_path(output_dir, "decoded_y_plane.png", png_path, sizeof(png_path))) {
        set_error(error_message, error_message_size, "failed to prepare decodePlane output paths");
        goto cleanup;
    }
    if (!write_raw_plane(raw_path, decoded, (size_t)width * height) ||
        !write_gray_png(factory, png_path, width, height, decoded, error_message, error_message_size)) {
        goto cleanup;
    }

    result->width = width;
    result->height = height;
    result->block_size = WIF_DECODE_BLOCK_SIZE;
    result->dtc_blocks = dtc_index;
    result->wave_blocks = wave_index;
    ok = 1;

cleanup:
    (void)route_byte_count;
    if (factory != NULL) {
        IWICImagingFactory_Release(factory);
    }
    if (initialized_com) {
        CoUninitialize();
    }
    free(route_map);
    free(dtc_plane);
    free(wave_plane);
    free(decoded);
    return ok;
}

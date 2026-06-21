#include "wif/evaluators/eye_entropy_evaluator.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <math.h>

#define WIF_BLOCK_SIZE 16

static void set_error(char *buffer, size_t buffer_size, const char *message)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static uint8_t clamp_u8_uint(uint32_t value)
{
    return value > 255 ? 255 : (uint8_t)value;
}

static int make_directory(const wchar_t *path, char *error_message, size_t error_message_size)
{
    if (CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 1;
    }

    set_error(error_message, error_message_size, "failed to create output directory");
    return 0;
}

static int join_path(
    const wchar_t *directory,
    const wchar_t *file_name,
    wchar_t *output,
    size_t output_count
)
{
    size_t directory_length = wcslen(directory);
    const wchar_t separator =
        directory_length > 0 && (directory[directory_length - 1] == L'\\' || directory[directory_length - 1] == L'/')
            ? L'\0'
            : L'\\';

    int written = separator == L'\0'
        ? swprintf(output, output_count, L"%ls%ls", directory, file_name)
        : swprintf(output, output_count, L"%ls%c%ls", directory, separator, file_name);

    return written > 0 && (size_t)written < output_count;
}

static int read_bytes(
    const wchar_t *path,
    uint8_t *bytes,
    size_t byte_count,
    char *error_message,
    size_t error_message_size
)
{
    FILE *file = NULL;
    long file_size;

    if (_wfopen_s(&file, path, L"rb") != 0 || file == NULL) {
        set_error(error_message, error_message_size, "failed to open raw input plane");
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        set_error(error_message, error_message_size, "failed to inspect raw input plane");
        return 0;
    }

    file_size = ftell(file);
    if (file_size < 0 || (size_t)file_size != byte_count) {
        fclose(file);
        set_error(error_message, error_message_size, "raw input plane size does not match width * height");
        return 0;
    }

    rewind(file);
    if (byte_count > 0 && fread(bytes, 1, byte_count, file) != byte_count) {
        fclose(file);
        set_error(error_message, error_message_size, "failed to read raw input plane");
        return 0;
    }

    fclose(file);
    return 1;
}

static int write_bytes(
    const wchar_t *path,
    const uint8_t *bytes,
    size_t byte_count,
    char *error_message,
    size_t error_message_size
)
{
    FILE *file = NULL;

    if (_wfopen_s(&file, path, L"wb") != 0 || file == NULL) {
        set_error(error_message, error_message_size, "failed to open raw output file");
        return 0;
    }

    if (byte_count > 0 && fwrite(bytes, 1, byte_count, file) != byte_count) {
        fclose(file);
        set_error(error_message, error_message_size, "failed to write raw output file");
        return 0;
    }

    fclose(file);
    return 1;
}

static int write_gray_png(
    IWICImagingFactory *factory,
    const wchar_t *path,
    uint32_t width,
    uint32_t height,
    const uint8_t *pixels,
    char *error_message,
    size_t error_message_size
)
{
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *property_bag = NULL;
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat8bppGray;
    HRESULT hr;

    hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create WIC stream");
        goto fail;
    }

    hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to open PNG output file");
        goto fail;
    }

    hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create PNG encoder");
        goto fail;
    }

    hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to initialize PNG encoder");
        goto fail;
    }

    hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &property_bag);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create PNG frame");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_Initialize(frame, property_bag);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to initialize PNG frame");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to set PNG size");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &pixel_format);
    if (FAILED(hr) || !IsEqualGUID(&pixel_format, &GUID_WICPixelFormat8bppGray)) {
        set_error(error_message, error_message_size, "failed to set grayscale PNG format");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_WritePixels(
        frame,
        height,
        width,
        width * height,
        (BYTE *)pixels
    );
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to write PNG pixels");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to commit PNG frame");
        goto fail;
    }

    hr = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to commit PNG file");
        goto fail;
    }

    if (property_bag != NULL) {
        IPropertyBag2_Release(property_bag);
    }
    IWICBitmapFrameEncode_Release(frame);
    IWICBitmapEncoder_Release(encoder);
    IWICStream_Release(stream);
    return 1;

fail:
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
    return 0;
}

static int write_bgra_png(
    IWICImagingFactory *factory,
    const wchar_t *path,
    uint32_t width,
    uint32_t height,
    const uint8_t *pixels,
    char *error_message,
    size_t error_message_size
)
{
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *property_bag = NULL;
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr;

    hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create WIC stream");
        goto fail;
    }

    hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to open BGRA PNG output file");
        goto fail;
    }

    hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create PNG encoder");
        goto fail;
    }

    hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to initialize PNG encoder");
        goto fail;
    }

    hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &property_bag);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create PNG frame");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_Initialize(frame, property_bag);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to initialize PNG frame");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to set PNG size");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &pixel_format);
    if (FAILED(hr) || !IsEqualGUID(&pixel_format, &GUID_WICPixelFormat32bppBGRA)) {
        set_error(error_message, error_message_size, "failed to set BGRA PNG format");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_WritePixels(
        frame,
        height,
        width * 4,
        width * height * 4,
        (BYTE *)pixels
    );
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to write BGRA PNG pixels");
        goto fail;
    }

    hr = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to commit PNG frame");
        goto fail;
    }

    hr = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to commit PNG file");
        goto fail;
    }

    if (property_bag != NULL) {
        IPropertyBag2_Release(property_bag);
    }
    IWICBitmapFrameEncode_Release(frame);
    IWICBitmapEncoder_Release(encoder);
    IWICStream_Release(stream);
    return 1;

fail:
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
    return 0;
}

static void build_edge_map(const uint8_t *plane, uint8_t *edge_map, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t index = (size_t)y * width + x;

            if (x == 0 || y == 0 || x + 1 >= width || y + 1 >= height) {
                edge_map[index] = 0;
            } else {
                int top_left = plane[(size_t)(y - 1) * width + (x - 1)];
                int top = plane[(size_t)(y - 1) * width + x];
                int top_right = plane[(size_t)(y - 1) * width + (x + 1)];
                int left = plane[(size_t)y * width + (x - 1)];
                int right = plane[(size_t)y * width + (x + 1)];
                int bottom_left = plane[(size_t)(y + 1) * width + (x - 1)];
                int bottom = plane[(size_t)(y + 1) * width + x];
                int bottom_right = plane[(size_t)(y + 1) * width + (x + 1)];
                int gx =
                    -top_left + top_right
                    -2 * left + 2 * right
                    -bottom_left + bottom_right;
                int gy =
                    top_left + 2 * top + top_right
                    -bottom_left - 2 * bottom - bottom_right;
                uint32_t edge = (uint32_t)floor(sqrt((double)gx * gx + (double)gy * gy) + 0.5);

                edge_map[index] = clamp_u8_uint(edge);
            }
        }
    }
}

static uint32_t ceil_sqrt_u32(uint32_t value)
{
    uint32_t side = 0;

    while (side * side < value) {
        ++side;
    }

    return side == 0 ? 1 : side;
}

static uint32_t source_block_count(uint32_t width, uint32_t height)
{
    uint32_t blocks_x = (width + WIF_BLOCK_SIZE - 1) / WIF_BLOCK_SIZE;
    uint32_t blocks_y = (height + WIF_BLOCK_SIZE - 1) / WIF_BLOCK_SIZE;

    return blocks_x * blocks_y;
}

static uint32_t compact_plane_size_for_blocks(uint32_t block_count)
{
    return ceil_sqrt_u32(block_count) * WIF_BLOCK_SIZE;
}

static void pack_route_bits(const uint8_t *route_map, uint32_t route_count, uint8_t *route_bits)
{
    for (uint32_t i = 0; i < route_count; ++i) {
        if (route_map[i]) {
            route_bits[i / 8] |= (uint8_t)(1u << (7 - (i % 8)));
        }
    }
}

static void classify_blocks(
    const uint8_t *plane,
    const uint8_t *edge_map,
    uint8_t *route_map,
    uint8_t *wave_dtc,
    uint32_t width,
    uint32_t height,
    uint8_t threshold,
    WifEyeEntropyResult *result
)
{
    for (uint32_t block_y = 0; block_y < height; block_y += WIF_BLOCK_SIZE) {
        uint32_t block_height = height - block_y < WIF_BLOCK_SIZE ? height - block_y : WIF_BLOCK_SIZE;

        for (uint32_t block_x = 0; block_x < width; block_x += WIF_BLOCK_SIZE) {
            uint32_t block_width = width - block_x < WIF_BLOCK_SIZE ? width - block_x : WIF_BLOCK_SIZE;
            uint8_t is_wave = 0;

            for (uint32_t y = 0; y < block_height; ++y) {
                for (uint32_t x = 0; x < block_width; ++x) {
                    size_t index = (size_t)(block_y + y) * width + (block_x + x);
                    if (edge_map[index] > threshold) {
                        is_wave = 1;
                        break;
                    }
                }
                if (is_wave) {
                    break;
                }
            }

            route_map[result->dtc_block_count + result->wave_block_count] = is_wave;

            if (is_wave) {
                ++result->wave_block_count;
            } else {
                ++result->dtc_block_count;
            }

            for (uint32_t y = 0; y < block_height; ++y) {
                for (uint32_t x = 0; x < block_width; ++x) {
                    size_t src_index = (size_t)(block_y + y) * width + (block_x + x);
                    size_t dst_index = src_index * 4;
                    uint8_t value = plane[src_index];

                    wave_dtc[dst_index + 0] = value;
                    wave_dtc[dst_index + 1] = value;
                    wave_dtc[dst_index + 2] = value;
                    wave_dtc[dst_index + 3] = is_wave ? 255 : 77;
                }
            }
        }
    }
}

static void pack_blocks(
    const uint8_t *plane,
    const uint8_t *route_map,
    uint8_t *dtc_plane,
    uint8_t *wave_plane,
    uint32_t width,
    uint32_t height,
    const WifEyeEntropyResult *result
)
{
    uint32_t block_index = 0;
    uint32_t dtc_index = 0;
    uint32_t wave_index = 0;
    uint32_t dtc_blocks_per_row = result->dtc_plane_width / WIF_BLOCK_SIZE;
    uint32_t wave_blocks_per_row = result->wave_plane_width / WIF_BLOCK_SIZE;

    for (uint32_t block_y = 0; block_y < height; block_y += WIF_BLOCK_SIZE) {
        uint32_t block_height = height - block_y < WIF_BLOCK_SIZE ? height - block_y : WIF_BLOCK_SIZE;

        for (uint32_t block_x = 0; block_x < width; block_x += WIF_BLOCK_SIZE) {
            uint32_t block_width = width - block_x < WIF_BLOCK_SIZE ? width - block_x : WIF_BLOCK_SIZE;
            uint8_t is_wave = route_map[block_index++];
            uint8_t *destination;
            uint32_t destination_width;
            uint32_t packed_index;
            uint32_t packed_block_x;
            uint32_t packed_block_y;

            if (is_wave) {
                destination = wave_plane;
                destination_width = result->wave_plane_width;
                packed_index = wave_index++;
                packed_block_x = (packed_index % wave_blocks_per_row) * WIF_BLOCK_SIZE;
                packed_block_y = (packed_index / wave_blocks_per_row) * WIF_BLOCK_SIZE;
            } else {
                destination = dtc_plane;
                destination_width = result->dtc_plane_width;
                packed_index = dtc_index++;
                packed_block_x = (packed_index % dtc_blocks_per_row) * WIF_BLOCK_SIZE;
                packed_block_y = (packed_index / dtc_blocks_per_row) * WIF_BLOCK_SIZE;
            }

            for (uint32_t y = 0; y < block_height; ++y) {
                for (uint32_t x = 0; x < block_width; ++x) {
                    size_t src_index = (size_t)(block_y + y) * width + (block_x + x);
                    size_t dst_index = (size_t)(packed_block_y + y) * destination_width + (packed_block_x + x);
                    destination[dst_index] = plane[src_index];
                }
            }
        }
    }
}

int wif_evaluate_raw_plane(
    const wchar_t *input_path,
    uint32_t width,
    uint32_t height,
    const wchar_t *output_dir,
    uint8_t threshold,
    WifEyeEntropyResult *result,
    char *error_message,
    size_t error_message_size
)
{
    IWICImagingFactory *factory = NULL;
    uint8_t *input_plane = NULL;
    uint8_t *edge_map = NULL;
    uint8_t *route_map = NULL;
    uint8_t *route_bits = NULL;
    uint8_t *wave_dtc = NULL;
    uint8_t *dtc_plane = NULL;
    uint8_t *wave_plane = NULL;
    wchar_t evaluator_dir[MAX_PATH];
    wchar_t edge_png_path[MAX_PATH];
    wchar_t edge_raw_path[MAX_PATH];
    wchar_t dtc_png_path[MAX_PATH];
    wchar_t dtc_raw_path[MAX_PATH];
    wchar_t wave_png_path[MAX_PATH];
    wchar_t wave_raw_path[MAX_PATH];
    wchar_t wave_dtc_path[MAX_PATH];
    wchar_t route_map_path[MAX_PATH];
    size_t plane_size;
    size_t rgba_plane_size;
    size_t route_bits_size;
    size_t dtc_plane_size;
    size_t wave_plane_size;
    uint32_t total_block_count;
    HRESULT hr;
    int initialized_com = 0;
    int ok = 0;

    if (input_path == NULL || output_dir == NULL || result == NULL || width == 0 || height == 0) {
        set_error(error_message, error_message_size, "invalid EyeEntropyEvaluator argument");
        return 0;
    }

    memset(result, 0, sizeof(*result));
    plane_size = (size_t)width * (size_t)height;
    rgba_plane_size = plane_size * 4;
    if (plane_size > UINT_MAX || rgba_plane_size / 4 != plane_size) {
        set_error(error_message, error_message_size, "raw plane is too large");
        return 0;
    }
    total_block_count = source_block_count(width, height);
    route_bits_size = ((size_t)total_block_count + 7) / 8;

    input_plane = (uint8_t *)malloc(plane_size);
    edge_map = (uint8_t *)malloc(plane_size);
    route_map = (uint8_t *)malloc(total_block_count);
    route_bits = (uint8_t *)calloc(route_bits_size, 1);
    wave_dtc = (uint8_t *)calloc(rgba_plane_size, 1);
    if (input_plane == NULL || edge_map == NULL || route_map == NULL || route_bits == NULL || wave_dtc == NULL) {
        set_error(error_message, error_message_size, "failed to allocate evaluator buffers");
        goto cleanup;
    }

    if (!read_bytes(input_path, input_plane, plane_size, error_message, error_message_size)) {
        goto cleanup;
    }

    result->width = width;
    result->height = height;
    result->block_size = WIF_BLOCK_SIZE;
    result->threshold = threshold;
    result->total_block_count = total_block_count;
    result->route_map_byte_count = (uint32_t)route_bits_size;

    build_edge_map(input_plane, edge_map, width, height);
    classify_blocks(input_plane, edge_map, route_map, wave_dtc, width, height, threshold, result);
    pack_route_bits(route_map, total_block_count, route_bits);

    result->dtc_plane_width = compact_plane_size_for_blocks(result->dtc_block_count);
    result->dtc_plane_height = result->dtc_plane_width;
    result->wave_plane_width = compact_plane_size_for_blocks(result->wave_block_count);
    result->wave_plane_height = result->wave_plane_width;

    dtc_plane_size = (size_t)result->dtc_plane_width * result->dtc_plane_height;
    wave_plane_size = (size_t)result->wave_plane_width * result->wave_plane_height;
    if (dtc_plane_size > UINT_MAX || wave_plane_size > UINT_MAX) {
        set_error(error_message, error_message_size, "compact plane is too large");
        goto cleanup;
    }

    dtc_plane = (uint8_t *)calloc(dtc_plane_size, 1);
    wave_plane = (uint8_t *)calloc(wave_plane_size, 1);
    if (dtc_plane == NULL || wave_plane == NULL) {
        set_error(error_message, error_message_size, "failed to allocate compact planes");
        goto cleanup;
    }

    pack_blocks(input_plane, route_map, dtc_plane, wave_plane, width, height, result);

    if (!make_directory(output_dir, error_message, error_message_size)) {
        goto cleanup;
    }
    if (!join_path(output_dir, L"eye_entropy", evaluator_dir, MAX_PATH)) {
        set_error(error_message, error_message_size, "output path is too long");
        goto cleanup;
    }
    if (!make_directory(evaluator_dir, error_message, error_message_size)) {
        goto cleanup;
    }
    if (!join_path(evaluator_dir, L"edge.png", edge_png_path, MAX_PATH) ||
        !join_path(evaluator_dir, L"edge.raw", edge_raw_path, MAX_PATH) ||
        !join_path(evaluator_dir, L"dtc_plane.png", dtc_png_path, MAX_PATH) ||
        !join_path(evaluator_dir, L"dtc_plane.raw", dtc_raw_path, MAX_PATH) ||
        !join_path(evaluator_dir, L"wave_plane.png", wave_png_path, MAX_PATH) ||
        !join_path(evaluator_dir, L"wave_plane.raw", wave_raw_path, MAX_PATH) ||
        !join_path(evaluator_dir, L"wave_dtc.png", wave_dtc_path, MAX_PATH) ||
        !join_path(evaluator_dir, L"route_map.bin", route_map_path, MAX_PATH)) {
        set_error(error_message, error_message_size, "output path is too long");
        goto cleanup;
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        initialized_com = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        set_error(error_message, error_message_size, "failed to initialize COM");
        goto cleanup;
    }

    hr = CoCreateInstance(
        &CLSID_WICImagingFactory,
        NULL,
        CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory,
        (LPVOID *)&factory
    );
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create WIC factory");
        goto cleanup;
    }

    if (!write_bytes(edge_raw_path, edge_map, plane_size, error_message, error_message_size) ||
        !write_bytes(route_map_path, route_bits, route_bits_size, error_message, error_message_size) ||
        !write_bytes(dtc_raw_path, dtc_plane, dtc_plane_size, error_message, error_message_size) ||
        !write_bytes(wave_raw_path, wave_plane, wave_plane_size, error_message, error_message_size) ||
        !write_gray_png(factory, edge_png_path, width, height, edge_map, error_message, error_message_size) ||
        !write_gray_png(factory, dtc_png_path, result->dtc_plane_width, result->dtc_plane_height, dtc_plane, error_message, error_message_size) ||
        !write_gray_png(factory, wave_png_path, result->wave_plane_width, result->wave_plane_height, wave_plane, error_message, error_message_size) ||
        !write_bgra_png(factory, wave_dtc_path, width, height, wave_dtc, error_message, error_message_size)) {
        goto cleanup;
    }

    ok = 1;

cleanup:
    if (factory != NULL) {
        IWICImagingFactory_Release(factory);
    }
    if (initialized_com) {
        CoUninitialize();
    }
    free(input_plane);
    free(edge_map);
    free(route_map);
    free(route_bits);
    free(wave_dtc);
    free(dtc_plane);
    free(wave_plane);
    return ok;
}

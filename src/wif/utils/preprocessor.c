#include "wif/utils/preprocessor.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>

static void set_error(char *buffer, size_t buffer_size, const char *message)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static uint8_t y_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return clamp_u8((77 * r + 150 * g + 29 * b + 128) >> 8);
}

static uint8_t cb_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return clamp_u8(128 + ((-43 * r - 85 * g + 128 * b + 128) >> 8));
}

static uint8_t cr_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return clamp_u8(128 + ((128 * r - 107 * g - 21 * b + 128) >> 8));
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

static int write_yuv444_planar(
    const wchar_t *path,
    const WifYuv444Image *image,
    size_t plane_size,
    char *error_message,
    size_t error_message_size
)
{
    FILE *file = NULL;

    if (_wfopen_s(&file, path, L"wb") != 0 || file == NULL) {
        set_error(error_message, error_message_size, "failed to open YUV444 output file");
        return 0;
    }

    if (fwrite(image->y_plane, 1, plane_size, file) != plane_size ||
        fwrite(image->cb_plane, 1, plane_size, file) != plane_size ||
        fwrite(image->cr_plane, 1, plane_size, file) != plane_size) {
        fclose(file);
        set_error(error_message, error_message_size, "failed to write YUV444 output file");
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

int wif_preprocess_image(
    const wchar_t *input_path,
    const wchar_t *output_dir,
    WifYuv444Image *image,
    char *error_message,
    size_t error_message_size
)
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *source_frame = NULL;
    IWICFormatConverter *converter = NULL;
    uint8_t *rgba = NULL;
    wchar_t preprocessed_dir[MAX_PATH];
    wchar_t y_path[MAX_PATH];
    wchar_t cb_path[MAX_PATH];
    wchar_t cr_path[MAX_PATH];
    wchar_t y_raw_path[MAX_PATH];
    wchar_t cb_raw_path[MAX_PATH];
    wchar_t cr_raw_path[MAX_PATH];
    wchar_t yuv444_path[MAX_PATH];
    UINT width = 0;
    UINT height = 0;
    size_t pixel_count;
    HRESULT hr;
    int initialized_com = 0;
    int ok = 0;

    if (input_path == NULL || output_dir == NULL || image == NULL) {
        set_error(error_message, error_message_size, "invalid preprocessor argument");
        return 0;
    }

    memset(image, 0, sizeof(*image));

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        initialized_com = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        set_error(error_message, error_message_size, "failed to initialize COM");
        return 0;
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

    hr = IWICImagingFactory_CreateDecoderFromFilename(
        factory,
        input_path,
        NULL,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder
    );
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to decode input image");
        goto cleanup;
    }

    hr = IWICBitmapDecoder_GetFrame(decoder, 0, &source_frame);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to read first image frame");
        goto cleanup;
    }

    hr = IWICBitmapFrameDecode_GetSize(source_frame, &width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        set_error(error_message, error_message_size, "input image has invalid dimensions");
        goto cleanup;
    }

    pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / 4 || pixel_count > UINT_MAX) {
        set_error(error_message, error_message_size, "input image is too large");
        goto cleanup;
    }

    hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to create pixel format converter");
        goto cleanup;
    }

    hr = IWICFormatConverter_Initialize(
        converter,
        (IWICBitmapSource *)source_frame,
        &GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        NULL,
        0.0,
        WICBitmapPaletteTypeCustom
    );
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to convert input image to RGBA");
        goto cleanup;
    }

    rgba = (uint8_t *)malloc(pixel_count * 4);
    image->y_plane = (uint8_t *)malloc(pixel_count);
    image->cb_plane = (uint8_t *)malloc(pixel_count);
    image->cr_plane = (uint8_t *)malloc(pixel_count);
    if (rgba == NULL || image->y_plane == NULL || image->cb_plane == NULL || image->cr_plane == NULL) {
        set_error(error_message, error_message_size, "failed to allocate plane buffers");
        goto cleanup;
    }

    hr = IWICFormatConverter_CopyPixels(
        converter,
        NULL,
        width * 4,
        pixel_count * 4,
        rgba
    );
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to copy RGBA pixels");
        goto cleanup;
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];

        image->y_plane[i] = y_from_rgb(r, g, b);
        image->cb_plane[i] = cb_from_rgb(r, g, b);
        image->cr_plane[i] = cr_from_rgb(r, g, b);
    }

    image->width = width;
    image->height = height;

    if (!make_directory(output_dir, error_message, error_message_size)) {
        goto cleanup;
    }
    if (!join_path(output_dir, L"preprocessed", preprocessed_dir, MAX_PATH)) {
        set_error(error_message, error_message_size, "output path is too long");
        goto cleanup;
    }
    if (!make_directory(preprocessed_dir, error_message, error_message_size)) {
        goto cleanup;
    }
    if (!join_path(preprocessed_dir, L"y_plane.png", y_path, MAX_PATH) ||
        !join_path(preprocessed_dir, L"cb_plane.png", cb_path, MAX_PATH) ||
        !join_path(preprocessed_dir, L"cr_plane.png", cr_path, MAX_PATH) ||
        !join_path(preprocessed_dir, L"y_plane_buffer.raw", y_raw_path, MAX_PATH) ||
        !join_path(preprocessed_dir, L"cb_plane_buffer.raw", cb_raw_path, MAX_PATH) ||
        !join_path(preprocessed_dir, L"cr_plane_buffer.raw", cr_raw_path, MAX_PATH) ||
        !join_path(preprocessed_dir, L"yuv444_planar.yuv", yuv444_path, MAX_PATH)) {
        set_error(error_message, error_message_size, "output path is too long");
        goto cleanup;
    }

    if (!write_gray_png(factory, y_path, width, height, image->y_plane, error_message, error_message_size) ||
        !write_gray_png(factory, cb_path, width, height, image->cb_plane, error_message, error_message_size) ||
        !write_gray_png(factory, cr_path, width, height, image->cr_plane, error_message, error_message_size)) {
        goto cleanup;
    }
    if (!write_bytes(y_raw_path, image->y_plane, pixel_count, error_message, error_message_size) ||
        !write_bytes(cb_raw_path, image->cb_plane, pixel_count, error_message, error_message_size) ||
        !write_bytes(cr_raw_path, image->cr_plane, pixel_count, error_message, error_message_size) ||
        !write_yuv444_planar(yuv444_path, image, pixel_count, error_message, error_message_size)) {
        goto cleanup;
    }

    ok = 1;

cleanup:
    free(rgba);
    if (!ok) {
        wif_free_yuv444_image(image);
    }
    if (converter != NULL) {
        IWICFormatConverter_Release(converter);
    }
    if (source_frame != NULL) {
        IWICBitmapFrameDecode_Release(source_frame);
    }
    if (decoder != NULL) {
        IWICBitmapDecoder_Release(decoder);
    }
    if (factory != NULL) {
        IWICImagingFactory_Release(factory);
    }
    if (initialized_com) {
        CoUninitialize();
    }
    return ok;
}

void wif_free_yuv444_image(WifYuv444Image *image)
{
    if (image == NULL) {
        return;
    }

    free(image->y_plane);
    free(image->cb_plane);
    free(image->cr_plane);
    memset(image, 0, sizeof(*image));
}

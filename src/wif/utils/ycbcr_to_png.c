#include "wif/utils/ycbcr_to_png.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static wchar_t *utf8_to_wide(const char *text)
{
    wchar_t *wide;
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);

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

static void ycbcr_to_bgra(
    const uint8_t *y_plane,
    const uint8_t *cb_plane,
    const uint8_t *cr_plane,
    uint8_t *bgra,
    size_t pixel_count
)
{
    for (size_t i = 0; i < pixel_count; ++i) {
        int y = y_plane[i];
        int cb = (int)cb_plane[i] - 128;
        int cr = (int)cr_plane[i] - 128;
        int r = y + ((359 * cr + 128) >> 8);
        int g = y - ((88 * cb + 183 * cr + 128) >> 8);
        int b = y + ((454 * cb + 128) >> 8);

        bgra[i * 4 + 0] = clamp_u8(b);
        bgra[i * 4 + 1] = clamp_u8(g);
        bgra[i * 4 + 2] = clamp_u8(r);
        bgra[i * 4 + 3] = 255;
    }
}

static int write_bgra_png(
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
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr;
    int ok = 0;

    if (wide_path == NULL) {
        set_error(error_message, error_message_size, "failed to convert output path");
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
    if (FAILED(hr) || !IsEqualGUID(&pixel_format, &GUID_WICPixelFormat32bppBGRA)) {
        set_error(error_message, error_message_size, "failed to set PNG pixel format");
        goto cleanup;
    }
    hr = IWICBitmapFrameEncode_WritePixels(frame, height, width * 4, width * height * 4, (BYTE *)pixels);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to write PNG pixels");
        goto cleanup;
    }
    hr = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to commit PNG frame");
        goto cleanup;
    }
    hr = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(hr)) {
        set_error(error_message, error_message_size, "failed to commit PNG");
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

int wif_ycbcr_raw_to_png(
    const char *y_path,
    const char *cb_path,
    const char *cr_path,
    uint32_t width,
    uint32_t height,
    const char *output_png_path,
    char *error_message,
    size_t error_message_size
)
{
    IWICImagingFactory *factory = NULL;
    uint8_t *y_plane = NULL;
    uint8_t *cb_plane = NULL;
    uint8_t *cr_plane = NULL;
    uint8_t *bgra = NULL;
    size_t pixel_count;
    HRESULT hr;
    int initialized_com = 0;
    int ok = 0;

    if (y_path == NULL || cb_path == NULL || cr_path == NULL || output_png_path == NULL || width == 0 || height == 0) {
        set_error(error_message, error_message_size, "invalid YCbCr to PNG argument");
        return 0;
    }

    pixel_count = (size_t)width * height;
    y_plane = (uint8_t *)malloc(pixel_count);
    cb_plane = (uint8_t *)malloc(pixel_count);
    cr_plane = (uint8_t *)malloc(pixel_count);
    bgra = (uint8_t *)malloc(pixel_count * 4);
    if (y_plane == NULL || cb_plane == NULL || cr_plane == NULL || bgra == NULL) {
        set_error(error_message, error_message_size, "failed to allocate YCbCr buffers");
        goto cleanup;
    }
    if (!read_raw_plane(y_path, y_plane, pixel_count) ||
        !read_raw_plane(cb_path, cb_plane, pixel_count) ||
        !read_raw_plane(cr_path, cr_plane, pixel_count)) {
        set_error(error_message, error_message_size, "failed to read YCbCr raw planes");
        goto cleanup;
    }

    ycbcr_to_bgra(y_plane, cb_plane, cr_plane, bgra, pixel_count);

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
    if (!write_bgra_png(factory, output_png_path, width, height, bgra, error_message, error_message_size)) {
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
    free(y_plane);
    free(cb_plane);
    free(cr_plane);
    free(bgra);
    return ok;
}

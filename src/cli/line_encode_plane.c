#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define BLOCK_SIZE 16

typedef struct {
    uint8_t *pixels;
    UINT width;
    UINT height;
} YPlane;

static wchar_t *utf8_to_wide(const char *text)
{
    wchar_t *wide;
    int count;

    if (text == NULL) return NULL;
    count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (count <= 0) return NULL;
    wide = (wchar_t *)calloc((size_t)count, sizeof(wchar_t));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)((299 * r + 587 * g + 114 * b + 500) / 1000);
}

static void print_usage(const char *program_name)
{
    fprintf(
        stderr,
        "usage: %s <input-image> [debug-root] [line-encoder-exe] [line-color-bits]\n"
        "default debug-root: data\\output\\debug\n"
        "output: <debug-root>\\<input-name>\\line_encoder\\...\n",
        program_name
    );
}

static int make_directory_recursive(const wchar_t *path)
{
    wchar_t buffer[MAX_PATH];
    size_t length;
    size_t i;

    if (path == NULL || path[0] == L'\0') return 0;
    if (wcsncpy_s(buffer, MAX_PATH, path, _TRUNCATE) != 0) return 0;
    length = wcslen(buffer);
    if (length == 0 || length >= MAX_PATH) return 0;

    for (i = 0; i < length; i++) {
        if ((buffer[i] == L'\\' || buffer[i] == L'/') && i > 0) {
            wchar_t saved = buffer[i];
            if (i == 2 && buffer[1] == L':') continue;
            buffer[i] = L'\0';
            if (buffer[0] != L'\0' &&
                !CreateDirectoryW(buffer, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                buffer[i] = saved;
                return 0;
            }
            buffer[i] = saved;
        }
    }
    if (!CreateDirectoryW(buffer, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    return 1;
}

static int path_join(wchar_t *out, size_t out_count, const wchar_t *left, const wchar_t *right)
{
    size_t left_len = wcslen(left);
    const wchar_t *separator = (left_len > 0 && (left[left_len - 1] == L'\\' || left[left_len - 1] == L'/'))
        ? L""
        : L"\\";
    return swprintf_s(out, out_count, L"%ls%ls%ls", left, separator, right) >= 0;
}

static int input_stem(const wchar_t *input_path, wchar_t *stem, size_t stem_count)
{
    const wchar_t *filename = wcsrchr(input_path, L'\\');
    wchar_t *extension;

    if (filename == NULL) filename = wcsrchr(input_path, L'/');
    filename = filename == NULL ? input_path : filename + 1;
    if (wcsncpy_s(stem, stem_count, filename, _TRUNCATE) != 0) return 0;
    extension = wcsrchr(stem, L'.');
    if (extension != NULL) *extension = L'\0';
    return stem[0] != L'\0';
}

static int default_sibling_exe_path(wchar_t *out, size_t out_count, const wchar_t *exe_name)
{
    wchar_t module_path[MAX_PATH];
    wchar_t *last_slash;

    if (GetModuleFileNameW(NULL, module_path, MAX_PATH) == 0) return 0;
    last_slash = wcsrchr(module_path, L'\\');
    if (last_slash == NULL) return 0;
    *last_slash = L'\0';
    return path_join(out, out_count, module_path, exe_name);
}

static int default_line_encoder_path(wchar_t *out, size_t out_count)
{
    return default_sibling_exe_path(out, out_count, L"wif-line-encoder.exe");
}

static int default_lines_binary_encoder_path(wchar_t *out, size_t out_count)
{
    return default_sibling_exe_path(out, out_count, L"wif-lines-binary-encoder-rans.exe");
}

static int load_image_y(const wchar_t *path, YPlane *plane)
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    uint8_t *bgra = NULL;
    HRESULT result;
    UINT width = 0;
    UINT height = 0;

    memset(plane, 0, sizeof(*plane));
    result = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateDecoderFromFilename(
        factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(result)) goto fail;
    result = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (FAILED(result)) goto fail;
    result = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    if (FAILED(result)) goto fail;
    result = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(result)) goto fail;
    result = IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame,
        &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(result)) goto fail;

    bgra = (uint8_t *)malloc((size_t)width * height * 4);
    plane->pixels = (uint8_t *)malloc((size_t)width * height);
    if (bgra == NULL || plane->pixels == NULL) goto fail;
    result = IWICFormatConverter_CopyPixels(converter, NULL, width * 4,
        (UINT)((size_t)width * height * 4), bgra);
    if (FAILED(result)) goto fail;

    for (UINT index = 0; index < width * height; index++) {
        plane->pixels[index] = rgb_to_y(
            bgra[index * 4 + 2],
            bgra[index * 4 + 1],
            bgra[index * 4 + 0]
        );
    }
    plane->width = width;
    plane->height = height;

    free(bgra);
    IWICFormatConverter_Release(converter);
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(decoder);
    IWICImagingFactory_Release(factory);
    return 1;

fail:
    free(bgra);
    free(plane->pixels);
    memset(plane, 0, sizeof(*plane));
    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (factory) IWICImagingFactory_Release(factory);
    return 0;
}

static int write_y_png(const wchar_t *path, const uint8_t *pixels, UINT width, UINT height)
{
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    uint8_t *bgr = NULL;
    HRESULT result;

    bgr = (uint8_t *)malloc((size_t)width * height * 3);
    if (bgr == NULL) return 0;
    for (UINT index = 0; index < width * height; index++) {
        bgr[index * 3 + 0] = pixels[index];
        bgr[index * 3 + 1] = pixels[index];
        bgr[index * 3 + 2] = pixels[index];
    }

    result = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory);
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
    result = IWICBitmapFrameEncode_WritePixels(frame, height, width * 3,
        (UINT)((size_t)width * height * 3), bgr);
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

static int write_block_png(const wchar_t *path, const YPlane *plane, UINT block_x, UINT block_y)
{
    uint8_t block[BLOCK_SIZE * BLOCK_SIZE];

    for (UINT y = 0; y < BLOCK_SIZE; y++) {
        memcpy(
            block + y * BLOCK_SIZE,
            plane->pixels + (block_y * BLOCK_SIZE + y) * plane->width + block_x * BLOCK_SIZE,
            BLOCK_SIZE
        );
    }
    return write_y_png(path, block, BLOCK_SIZE, BLOCK_SIZE);
}

static int copy_block_to_plane(uint8_t *target, UINT target_width, UINT block_x, UINT block_y, const YPlane *block)
{
    if (block->width != BLOCK_SIZE || block->height != BLOCK_SIZE) return 0;
    for (UINT y = 0; y < BLOCK_SIZE; y++) {
        memcpy(
            target + (block_y * BLOCK_SIZE + y) * target_width + block_x * BLOCK_SIZE,
            block->pixels + y * BLOCK_SIZE,
            BLOCK_SIZE
        );
    }
    return 1;
}

static int build_block_output_path(
    wchar_t *out,
    size_t out_count,
    const wchar_t *files_root,
    UINT block_y,
    UINT block_x,
    const wchar_t *filename
)
{
    wchar_t block_name[64];
    wchar_t block_dir[MAX_PATH];

    if (swprintf_s(block_name, 64, L"block_%u_%u", block_y, block_x) < 0) return 0;
    return path_join(block_dir, MAX_PATH, files_root, block_name) &&
        path_join(out, out_count, block_dir, filename);
}

static int run_line_encoder(
    const wchar_t *line_encoder_exe,
    const wchar_t *block_path,
    const wchar_t *files_root,
    int line_color_bits
)
{
    wchar_t command[4096];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HANDLE null_output = INVALID_HANDLE_VALUE;
    DWORD exit_code = 1;

    if (swprintf_s(
            command,
            4096,
            L"\"%ls\" \"%ls\" \"%ls\" 0 %d",
            line_encoder_exe,
            block_path,
            files_root,
            line_color_bits) < 0) {
        return 0;
    }

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    null_output = CreateFileW(
        L"NUL",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (null_output != INVALID_HANDLE_VALUE) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = null_output;
        startup.hStdError = null_output;
    }

    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        if (null_output != INVALID_HANDLE_VALUE) CloseHandle(null_output);
        return 0;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (null_output != INVALID_HANDLE_VALUE) CloseHandle(null_output);
    return exit_code == 0;
}

static int run_lines_binary_encoder(
    const wchar_t *binary_encoder_exe,
    const wchar_t *files_root,
    const wchar_t *binary_path,
    const wchar_t *report_path
)
{
    wchar_t command[4096];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 1;

    if (swprintf_s(
            command,
            4096,
            L"\"%ls\" \"%ls\" \"%ls\" \"%ls\"",
            binary_encoder_exe,
            files_root,
            binary_path,
            report_path) < 0) {
        return 0;
    }

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);

    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        return 0;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

int main(int argc, char **argv)
{
    wchar_t *input_path = NULL;
    wchar_t *debug_root = NULL;
    wchar_t *provided_encoder = NULL;
    wchar_t default_debug_root[] = L"data\\output\\debug";
    wchar_t line_encoder_exe[MAX_PATH];
    wchar_t lines_binary_encoder_exe[MAX_PATH];
    wchar_t name[MAX_PATH];
    wchar_t image_root[MAX_PATH];
    wchar_t output_root[MAX_PATH];
    wchar_t blocks_root[MAX_PATH];
    wchar_t files_root[MAX_PATH];
    wchar_t y_plane_path[MAX_PATH];
    wchar_t full_residue_path[MAX_PATH];
    wchar_t lines_binary_path[MAX_PATH];
    wchar_t lines_binary_report_path[MAX_PATH];
    YPlane plane;
    uint8_t *full_residue = NULL;
    UINT blocks_x;
    UINT blocks_y;
    UINT block_count = 0;
    int line_color_bits = 4;
    HRESULT co_result;
    int exit_code = 1;

    memset(&plane, 0, sizeof(plane));
    if (argc < 2 || argc > 5) {
        print_usage(argv[0]);
        return 2;
    }

    input_path = utf8_to_wide(argv[1]);
    debug_root = argc >= 3 ? utf8_to_wide(argv[2]) : _wcsdup(default_debug_root);
    provided_encoder = argc >= 4 ? utf8_to_wide(argv[3]) : NULL;
    if (argc >= 5) {
        line_color_bits = atoi(argv[4]);
        if (line_color_bits != 2 && line_color_bits != 4 && line_color_bits != 6) {
            fprintf(stderr, "line-color-bits must be 2, 4, or 6\n");
            return 2;
        }
    }
    if (input_path == NULL || debug_root == NULL || (argc >= 4 && provided_encoder == NULL)) {
        fprintf(stderr, "failed to read command line paths\n");
        goto cleanup;
    }

    if (provided_encoder != NULL) {
        if (wcsncpy_s(line_encoder_exe, MAX_PATH, provided_encoder, _TRUNCATE) != 0) goto cleanup;
    } else if (!default_line_encoder_path(line_encoder_exe, MAX_PATH)) {
        fprintf(stderr, "failed to locate wif-line-encoder.exe beside this CLI\n");
        goto cleanup;
    }
    if (!default_lines_binary_encoder_path(lines_binary_encoder_exe, MAX_PATH)) {
        fprintf(stderr, "failed to locate wif-lines-binary-encoder-rans.exe beside this CLI\n");
        goto cleanup;
    }

    if (!input_stem(input_path, name, MAX_PATH) ||
        !path_join(image_root, MAX_PATH, debug_root, name) ||
        !path_join(output_root, MAX_PATH, image_root, L"line_encoder") ||
        !path_join(blocks_root, MAX_PATH, output_root, L"blocks") ||
        !path_join(files_root, MAX_PATH, output_root, L"files") ||
        !path_join(y_plane_path, MAX_PATH, output_root, L"y_plane.png") ||
        !path_join(full_residue_path, MAX_PATH, output_root, L"full_residue.png") ||
        !path_join(lines_binary_path, MAX_PATH, output_root, L"lines_binary.rans") ||
        !path_join(lines_binary_report_path, MAX_PATH, output_root, L"lines_binary_rans_report.json")) {
        fprintf(stderr, "failed to build output paths\n");
        goto cleanup;
    }

    co_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(co_result) && co_result != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "failed to initialize COM\n");
        goto cleanup;
    }

    if (!make_directory_recursive(blocks_root) || !make_directory_recursive(files_root)) {
        fprintf(stderr, "failed to create output directories\n");
        goto cleanup_com;
    }
    if (!load_image_y(input_path, &plane)) {
        fprintf(stderr, "failed to load input image and extract Y channel\n");
        goto cleanup_com;
    }
    if (plane.width % BLOCK_SIZE != 0 || plane.height % BLOCK_SIZE != 0) {
        fprintf(stderr, "image dimensions must be divisible by 16, got %u x %u\n", plane.width, plane.height);
        goto cleanup_com;
    }
    if (!write_y_png(y_plane_path, plane.pixels, plane.width, plane.height)) {
        fprintf(stderr, "failed to write extracted Y plane PNG\n");
        goto cleanup_com;
    }
    full_residue = (uint8_t *)malloc((size_t)plane.width * plane.height);
    if (full_residue == NULL) {
        fprintf(stderr, "failed to allocate full residue image\n");
        goto cleanup_com;
    }

    blocks_x = plane.width / BLOCK_SIZE;
    blocks_y = plane.height / BLOCK_SIZE;
    for (UINT by = 0; by < blocks_y; by++) {
        for (UINT bx = 0; bx < blocks_x; bx++) {
            wchar_t filename[64];
            wchar_t block_path[MAX_PATH];

            if (swprintf_s(filename, 64, L"block_%u_%u.png", by, bx) < 0 ||
                !path_join(block_path, MAX_PATH, blocks_root, filename)) {
                fprintf(stderr, "failed to build block path\n");
                goto cleanup_com;
            }
            if (!write_block_png(block_path, &plane, bx, by)) {
                fwprintf(stderr, L"failed to write block PNG: %ls\n", block_path);
                goto cleanup_com;
            }
            if (!run_line_encoder(line_encoder_exe, block_path, files_root, line_color_bits)) {
                fwprintf(stderr, L"line encoder failed for block: %ls\n", block_path);
                goto cleanup_com;
            }
            {
                wchar_t residue_block_path[MAX_PATH];
                YPlane residue_block;

                memset(&residue_block, 0, sizeof(residue_block));
                if (!build_block_output_path(
                        residue_block_path,
                        MAX_PATH,
                        files_root,
                        by,
                        bx,
                        L"residue.png") ||
                    !load_image_y(residue_block_path, &residue_block) ||
                    !copy_block_to_plane(full_residue, plane.width, bx, by, &residue_block)) {
                    free(residue_block.pixels);
                    fwprintf(stderr, L"failed to read residue block: %ls\n", residue_block_path);
                    goto cleanup_com;
                }
                free(residue_block.pixels);
            }
            block_count++;
            if (block_count % 256 == 0) {
                printf("processed %u / %u blocks\n", block_count, blocks_x * blocks_y);
            }
        }
    }
    if (!write_y_png(full_residue_path, full_residue, plane.width, plane.height)) {
        fprintf(stderr, "failed to write full residue PNG\n");
        goto cleanup_com;
    }

    if (!run_lines_binary_encoder(
            lines_binary_encoder_exe,
            files_root,
            lines_binary_path,
            lines_binary_report_path)) {
        fprintf(stderr, "lines binary rANS encoder failed\n");
        goto cleanup_com;
    }

    printf("input: %u x %u\n", plane.width, plane.height);
    printf("blocks: %u x %u (%u total)\n", blocks_x, blocks_y, block_count);
    wprintf(L"line encoder output: %ls\n", output_root);
    wprintf(L"full residue: %ls\n", full_residue_path);
    wprintf(L"lines binary: %ls\n", lines_binary_path);
    wprintf(L"lines binary report: %ls\n", lines_binary_report_path);
    exit_code = 0;

cleanup_com:
    free(full_residue);
    free(plane.pixels);
    CoUninitialize();
cleanup:
    free(input_path);
    free(debug_root);
    free(provided_encoder);
    return exit_code;
}

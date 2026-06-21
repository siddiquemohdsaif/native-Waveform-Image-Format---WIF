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
#define FAIL_ALPHA 77
#define PASS_ALPHA 255

typedef struct {
    wchar_t name[MAX_PATH];
    int row;
    int column;
    double energy;
    double energy_concentration_percent;
    int passed;
    int more_relax_passed;
} BlockAnalysis;

typedef struct {
    uint8_t bgra[BLOCK_SIZE * BLOCK_SIZE * 4];
} BlockPixels;

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

static void print_usage(const char *program_name)
{
    fprintf(stderr, "usage: %s <line-encoder-files-directory> [output-json]\n", program_name);
}

static char *read_text_file(const wchar_t *path)
{
    FILE *file = NULL;
    long length;
    char *text;

    if (_wfopen_s(&file, path, L"rb") != 0 || file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(text, 1, (size_t)length, file) != (size_t)length) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[length] = '\0';
    fclose(file);
    return text;
}

static int parse_json_double(const char *json, const char *key, double *value)
{
    const char *cursor = strstr(json, key);
    char *end;

    if (cursor == NULL) return 0;
    cursor += strlen(key);
    while (*cursor == ' ' || *cursor == ':') cursor++;
    *value = strtod(cursor, &end);
    return end != cursor;
}

static int append_block(BlockAnalysis **blocks, size_t *count, size_t *capacity, const BlockAnalysis *block)
{
    if (*count == *capacity) {
        size_t next_capacity = *capacity == 0 ? 256 : *capacity * 2;
        BlockAnalysis *next = (BlockAnalysis *)realloc(*blocks, next_capacity * sizeof(**blocks));
        if (next == NULL) return 0;
        *blocks = next;
        *capacity = next_capacity;
    }
    (*blocks)[(*count)++] = *block;
    return 1;
}

static int compare_blocks(const void *left, const void *right)
{
    const BlockAnalysis *a = (const BlockAnalysis *)left;
    const BlockAnalysis *b = (const BlockAnalysis *)right;

    if (a->energy < b->energy) return 1;
    if (a->energy > b->energy) return -1;
    if (a->energy_concentration_percent < b->energy_concentration_percent) return 1;
    if (a->energy_concentration_percent > b->energy_concentration_percent) return -1;
    if (a->row != b->row) return a->row - b->row;
    return a->column - b->column;
}

static int block_passes_rules(double energy, double concentration)
{
    if (concentration >= 97.0) return 0;
    if (energy >= 90.0 && concentration >= 50.0) return 1;
    if (energy >= 80.0 && concentration >= 60.0) return 1;
    if (energy >= 70.0 && concentration >= 70.0) return 1;
    if (energy >= 60.0 && concentration >= 80.0) return 1;
    if (energy >= 50.0 && concentration >= 80.0) return 1;
    if (energy >= 40.0 && concentration >= 80.0) return 1;
    if (energy >= 30.0 && concentration >= 80.0) return 1;
    if (energy >= 20.0 && concentration >= 85.0) return 1;
    if (energy >= 10.0 && concentration >= 85.0) return 1;
    if (energy < 10.0) return 0;
    return 0;
}

static int block_passes_more_relax_rules(double energy, double concentration)
{
    if (concentration >= 97.0) return 0;
    if (energy >= 90.0 && concentration >= 45.0) return 1;
    if (energy >= 80.0 && concentration >= 50.0) return 1;
    if (energy >= 70.0 && concentration >= 60.0) return 1;
    if (energy >= 60.0 && concentration >= 65.0) return 1;
    if (energy >= 50.0 && concentration >= 65.0) return 1;
    if (energy >= 40.0 && concentration >= 70.0) return 1;
    if (energy >= 30.0 && concentration >= 70.0) return 1;
    if (energy >= 20.0 && concentration >= 70.0) return 1;
    if (energy >= 10.0 && concentration >= 75.0) return 1;
    if (energy < 10.0) return 0;
    return 0;
}

static int collect_blocks(const wchar_t *files_root, BlockAnalysis **blocks_out, size_t *count_out)
{
    WIN32_FIND_DATAW find_data;
    HANDLE handle;
    wchar_t pattern[MAX_PATH];
    BlockAnalysis *blocks = NULL;
    size_t count = 0;
    size_t capacity = 0;

    if (swprintf_s(pattern, MAX_PATH, L"%ls\\block_*_*", files_root) < 0) return 0;
    handle = FindFirstFileW(pattern, &find_data);
    if (handle == INVALID_HANDLE_VALUE) return 0;

    do {
        wchar_t json_path[MAX_PATH];
        BlockAnalysis block;
        char *json;

        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        memset(&block, 0, sizeof(block));
        if (swscanf_s(find_data.cFileName, L"block_%d_%d", &block.row, &block.column) != 2) continue;
        if (wcsncpy_s(block.name, MAX_PATH, find_data.cFileName, _TRUNCATE) != 0) {
            FindClose(handle);
            free(blocks);
            return 0;
        }
        if (swprintf_s(json_path, MAX_PATH, L"%ls\\%ls\\horizontal_vertical_lines.json", files_root, find_data.cFileName) < 0) {
            FindClose(handle);
            free(blocks);
            return 0;
        }
        json = read_text_file(json_path);
        if (json == NULL) continue;
        if (!parse_json_double(json, "\"energy\"", &block.energy) ||
            !parse_json_double(json, "\"energyConcentrationPercent\"", &block.energy_concentration_percent)) {
            free(json);
            FindClose(handle);
            free(blocks);
            return 0;
        }
        block.passed = block_passes_rules(block.energy, block.energy_concentration_percent);
        block.more_relax_passed = block_passes_more_relax_rules(block.energy, block.energy_concentration_percent);
        free(json);
        if (!append_block(&blocks, &count, &capacity, &block)) {
            FindClose(handle);
            free(blocks);
            return 0;
        }
    } while (FindNextFileW(handle, &find_data));

    FindClose(handle);
    qsort(blocks, count, sizeof(blocks[0]), compare_blocks);
    *blocks_out = blocks;
    *count_out = count;
    return 1;
}

static int write_report(const wchar_t *path, const BlockAnalysis *blocks, size_t count)
{
    FILE *file = NULL;
    size_t pass_count = 0;
    size_t fail_count = 0;
    size_t more_relax_pass_count = 0;
    size_t more_relax_fail_count = 0;

    for (size_t index = 0; index < count; index++) {
        if (blocks[index].passed) pass_count++;
        else fail_count++;
        if (blocks[index].more_relax_passed) more_relax_pass_count++;
        else more_relax_fail_count++;
    }
    if (_wfopen_s(&file, path, L"wb") != 0 || file == NULL) return 0;
    fprintf(file, "{\n");
    fprintf(file, "  \"sort\": [\"energy desc\", \"energyConcentrationPercent desc\"],\n");
    fprintf(file, "  \"blockCount\": %zu,\n", count);
    fprintf(file, "  \"passRule\": {\n");
    fprintf(file, "    \"forceFail\": [\"concentration >= 97\", \"energy < 10\"],\n");
    fprintf(file, "    \"pass\": [\n");
    fprintf(file, "      \"energy >= 90 and concentration >= 50\",\n");
    fprintf(file, "      \"energy >= 80 and concentration >= 60\",\n");
    fprintf(file, "      \"energy >= 70 and concentration >= 70\",\n");
    fprintf(file, "      \"energy >= 60 and concentration >= 80\",\n");
    fprintf(file, "      \"energy >= 50 and concentration >= 80\",\n");
    fprintf(file, "      \"energy >= 40 and concentration >= 80\",\n");
    fprintf(file, "      \"energy >= 30 and concentration >= 80\",\n");
    fprintf(file, "      \"energy >= 20 and concentration >= 85\",\n");
    fprintf(file, "      \"energy >= 10 and concentration >= 85\"\n");
    fprintf(file, "    ]\n");
    fprintf(file, "  },\n");
    fprintf(file, "  \"passCount\": %zu,\n", pass_count);
    fprintf(file, "  \"failCount\": %zu,\n", fail_count);
    fprintf(file, "  \"moreRelaxPassRule\": {\n");
    fprintf(file, "    \"forceFail\": [\"concentration >= 97\", \"energy < 10\"],\n");
    fprintf(file, "    \"pass\": [\n");
    fprintf(file, "      \"energy >= 90 and concentration >= 45\",\n");
    fprintf(file, "      \"energy >= 80 and concentration >= 50\",\n");
    fprintf(file, "      \"energy >= 70 and concentration >= 60\",\n");
    fprintf(file, "      \"energy >= 60 and concentration >= 65\",\n");
    fprintf(file, "      \"energy >= 50 and concentration >= 65\",\n");
    fprintf(file, "      \"energy >= 40 and concentration >= 70\",\n");
    fprintf(file, "      \"energy >= 30 and concentration >= 70\",\n");
    fprintf(file, "      \"energy >= 20 and concentration >= 70\",\n");
    fprintf(file, "      \"energy >= 10 and concentration >= 75\"\n");
    fprintf(file, "    ]\n");
    fprintf(file, "  },\n");
    fprintf(file, "  \"moreRelaxPassCount\": %zu,\n", more_relax_pass_count);
    fprintf(file, "  \"moreRelaxFailCount\": %zu,\n", more_relax_fail_count);
    fprintf(file, "  \"blocks\": {\n");
    for (size_t index = 0; index < count; index++) {
        fprintf(
            file,
            "    \"%ls\": {\"energy\": %.6f, \"energyConcentrationPercent\": %.6f, \"result\": \"%s\", \"moreRelaxResult\": \"%s\"}%s\n",
            blocks[index].name,
            blocks[index].energy,
            blocks[index].energy_concentration_percent,
            blocks[index].passed ? "pass" : "fail",
            blocks[index].more_relax_passed ? "pass" : "fail",
            index + 1 == count ? "" : ","
        );
    }
    fprintf(file, "  },\n");
    fprintf(file, "  \"passBlocks\": {\n");
    {
        size_t written = 0;
        for (size_t index = 0; index < count; index++) {
            if (!blocks[index].passed) continue;
            fprintf(
                file,
                "    \"%ls\": {\"energy\": %.6f, \"energyConcentrationPercent\": %.6f}%s\n",
                blocks[index].name,
                blocks[index].energy,
                blocks[index].energy_concentration_percent,
                ++written == pass_count ? "" : ","
            );
        }
    }
    fprintf(file, "  },\n");
    fprintf(file, "  \"failBlocks\": {\n");
    {
        size_t written = 0;
        for (size_t index = 0; index < count; index++) {
            if (blocks[index].passed) continue;
            fprintf(
                file,
                "    \"%ls\": {\"energy\": %.6f, \"energyConcentrationPercent\": %.6f}%s\n",
                blocks[index].name,
                blocks[index].energy,
                blocks[index].energy_concentration_percent,
                ++written == fail_count ? "" : ","
            );
        }
    }
    fprintf(file, "  },\n");
    fprintf(file, "  \"moreRelaxPassBlocks\": {\n");
    {
        size_t written = 0;
        for (size_t index = 0; index < count; index++) {
            if (!blocks[index].more_relax_passed) continue;
            fprintf(
                file,
                "    \"%ls\": {\"energy\": %.6f, \"energyConcentrationPercent\": %.6f}%s\n",
                blocks[index].name,
                blocks[index].energy,
                blocks[index].energy_concentration_percent,
                ++written == more_relax_pass_count ? "" : ","
            );
        }
    }
    fprintf(file, "  },\n");
    fprintf(file, "  \"moreRelaxFailBlocks\": {\n");
    {
        size_t written = 0;
        for (size_t index = 0; index < count; index++) {
            if (blocks[index].more_relax_passed) continue;
            fprintf(
                file,
                "    \"%ls\": {\"energy\": %.6f, \"energyConcentrationPercent\": %.6f}%s\n",
                blocks[index].name,
                blocks[index].energy,
                blocks[index].energy_concentration_percent,
                ++written == more_relax_fail_count ? "" : ","
            );
        }
    }
    fprintf(file, "  }\n");
    fprintf(file, "}\n");
    fclose(file);
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

static int load_block_bgra(const wchar_t *path, BlockPixels *block)
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    UINT width = 0;
    UINT height = 0;
    HRESULT result;

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
    if (width != BLOCK_SIZE || height != BLOCK_SIZE) goto fail;
    result = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(result)) goto fail;
    result = IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame,
        &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(result)) goto fail;
    result = IWICFormatConverter_CopyPixels(
        converter, NULL, BLOCK_SIZE * 4, sizeof(block->bgra), block->bgra);
    if (FAILED(result)) goto fail;

    IWICFormatConverter_Release(converter);
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(decoder);
    IWICImagingFactory_Release(factory);
    return 1;

fail:
    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (factory) IWICImagingFactory_Release(factory);
    return 0;
}

static int write_bgra_png(const wchar_t *path, const uint8_t *pixels, UINT width, UINT height)
{
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT result;

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
    result = IWICBitmapFrameEncode_WritePixels(frame, height, width * 4, width * height * 4, (BYTE *)pixels);
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
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    return 0;
}

static int write_alpha_image(
    const wchar_t *path,
    const wchar_t *files_root,
    const BlockAnalysis *blocks,
    size_t count,
    int use_more_relax_rule
)
{
    int max_row = -1;
    int max_column = -1;
    UINT width;
    UINT height;
    uint8_t *pixels;

    for (size_t index = 0; index < count; index++) {
        if (blocks[index].row > max_row) max_row = blocks[index].row;
        if (blocks[index].column > max_column) max_column = blocks[index].column;
    }
    if (max_row < 0 || max_column < 0) return 0;
    width = (UINT)(max_column + 1) * BLOCK_SIZE;
    height = (UINT)(max_row + 1) * BLOCK_SIZE;
    pixels = (uint8_t *)calloc((size_t)width * height * 4, 1);
    if (pixels == NULL) return 0;

    for (size_t index = 0; index < count; index++) {
        wchar_t block_dir[MAX_PATH];
        wchar_t original_path[MAX_PATH];
        BlockPixels block;
        uint8_t alpha = (use_more_relax_rule ? blocks[index].more_relax_passed : blocks[index].passed)
            ? PASS_ALPHA
            : FAIL_ALPHA;

        if (!path_join(block_dir, MAX_PATH, files_root, blocks[index].name) ||
            !path_join(original_path, MAX_PATH, block_dir, L"original.png") ||
            !load_block_bgra(original_path, &block)) {
            free(pixels);
            return 0;
        }
        for (UINT y = 0; y < BLOCK_SIZE; y++) {
            for (UINT x = 0; x < BLOCK_SIZE; x++) {
                size_t source = (size_t)(y * BLOCK_SIZE + x) * 4;
                size_t target = ((size_t)(blocks[index].row * BLOCK_SIZE + y) * width +
                    (size_t)(blocks[index].column * BLOCK_SIZE + x)) * 4;
                pixels[target + 0] = block.bgra[source + 0];
                pixels[target + 1] = block.bgra[source + 1];
                pixels[target + 2] = block.bgra[source + 2];
                pixels[target + 3] = alpha;
            }
        }
    }

    if (!write_bgra_png(path, pixels, width, height)) {
        free(pixels);
        return 0;
    }
    free(pixels);
    return 1;
}

static int directory_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int main(int argc, char **argv)
{
    wchar_t *input_root = NULL;
    wchar_t files_root[MAX_PATH];
    wchar_t output_root[MAX_PATH];
    wchar_t *output_path = NULL;
    wchar_t *image_output_path = NULL;
    wchar_t *more_relax_image_output_path = NULL;
    wchar_t default_output[MAX_PATH];
    wchar_t default_image_output[MAX_PATH];
    wchar_t default_more_relax_image_output[MAX_PATH];
    BlockAnalysis *blocks = NULL;
    size_t count = 0;
    HRESULT co_result;
    int exit_code = 1;

    if (argc < 2 || argc > 4) {
        print_usage(argv[0]);
        return 2;
    }

    input_root = utf8_to_wide(argv[1]);
    output_path = argc == 3 ? utf8_to_wide(argv[2]) : NULL;
    image_output_path = argc == 4 ? utf8_to_wide(argv[3]) : NULL;
    if (input_root == NULL || (argc >= 3 && output_path == NULL) || (argc == 4 && image_output_path == NULL)) {
        fprintf(stderr, "failed to read command line paths\n");
        goto cleanup;
    }
    if (directory_exists(input_root)) {
        wchar_t candidate_files[MAX_PATH];
        if (!path_join(candidate_files, MAX_PATH, input_root, L"files")) {
            fprintf(stderr, "failed to build files path\n");
            goto cleanup;
        }
        if (directory_exists(candidate_files)) {
            if (wcsncpy_s(files_root, MAX_PATH, candidate_files, _TRUNCATE) != 0 ||
                wcsncpy_s(output_root, MAX_PATH, input_root, _TRUNCATE) != 0) {
                fprintf(stderr, "failed to copy root paths\n");
                goto cleanup;
            }
        } else {
            const wchar_t *last_slash;
            if (wcsncpy_s(files_root, MAX_PATH, input_root, _TRUNCATE) != 0) {
                fprintf(stderr, "failed to copy files path\n");
                goto cleanup;
            }
            last_slash = wcsrchr(input_root, L'\\');
            if (last_slash == NULL) last_slash = wcsrchr(input_root, L'/');
            if (last_slash != NULL && wcscmp(last_slash + 1, L"files") == 0) {
                size_t parent_length = (size_t)(last_slash - input_root);
                if (parent_length >= MAX_PATH) {
                    fprintf(stderr, "parent output path is too long\n");
                    goto cleanup;
                }
                wmemcpy(output_root, input_root, parent_length);
                output_root[parent_length] = L'\0';
            } else if (wcsncpy_s(output_root, MAX_PATH, input_root, _TRUNCATE) != 0) {
                fprintf(stderr, "failed to copy output path\n");
                goto cleanup;
            }
        }
    } else {
        fprintf(stderr, "input directory does not exist\n");
        goto cleanup;
    }
    if (output_path == NULL) {
        if (swprintf_s(default_output, MAX_PATH, L"%ls\\wave_block_analysis.json", output_root) < 0) {
            fprintf(stderr, "failed to build default output path\n");
            goto cleanup;
        }
        output_path = _wcsdup(default_output);
        if (output_path == NULL) goto cleanup;
    }
    if (image_output_path == NULL) {
        if (swprintf_s(default_image_output, MAX_PATH, L"%ls\\wave_block_pass_fail_alpha.png", output_root) < 0) {
            fprintf(stderr, "failed to build default image output path\n");
            goto cleanup;
        }
        image_output_path = _wcsdup(default_image_output);
        if (image_output_path == NULL) goto cleanup;
    }
    if (swprintf_s(default_more_relax_image_output, MAX_PATH, L"%ls\\wave_block_more_relax_alpha.png", output_root) < 0) {
        fprintf(stderr, "failed to build more-relax image output path\n");
        goto cleanup;
    }
    more_relax_image_output_path = _wcsdup(default_more_relax_image_output);
    if (more_relax_image_output_path == NULL) goto cleanup;

    co_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(co_result) && co_result != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "failed to initialize COM\n");
        goto cleanup;
    }

    if (!collect_blocks(files_root, &blocks, &count)) {
        fprintf(stderr, "failed to analyze block JSON files\n");
        goto cleanup_com;
    }
    if (!write_report(output_path, blocks, count)) {
        fprintf(stderr, "failed to write analysis JSON\n");
        goto cleanup_com;
    }
    if (!write_alpha_image(image_output_path, files_root, blocks, count, 0)) {
        fprintf(stderr, "failed to write pass/fail alpha PNG\n");
        goto cleanup_com;
    }
    if (!write_alpha_image(more_relax_image_output_path, files_root, blocks, count, 1)) {
        fprintf(stderr, "failed to write more-relax alpha PNG\n");
        goto cleanup_com;
    }

    printf("analyzed %zu blocks\n", count);
    wprintf(L"analysis: %ls\n", output_path);
    wprintf(L"pass/fail alpha image: %ls\n", image_output_path);
    wprintf(L"more-relax alpha image: %ls\n", more_relax_image_output_path);
    exit_code = 0;

cleanup_com:
    CoUninitialize();
cleanup:
    free(input_root);
    free(output_path);
    free(image_output_path);
    free(more_relax_image_output_path);
    free(blocks);
    return exit_code;
}

# Project Structure

Recommended WIF project layout:

```text
native-Waveform-Image-Format---WIF/
  BLUEPRINT.md
  PROJECT_STRUCTURE.md
  src/
    wif/
      core/
      encoders/
      evaluators/
      format/
      image/
      pipeline/
      utils/
    cli/
  data/
    input/
      images/
      yuv/
    output/
      wif/
      debug/
      decoded/
    fixtures/
  tests/
    unit/
    integration/
  docs/
    specs/
    diagrams/
  tools/
  build/
```

## Source Code

- `src/wif/core/`: shared block, plane, and binary data structures.
- `src/wif/image/`: image loading, PNG decoding, and RGB-to-YUV444 conversion.
- `src/wif/evaluators/`: `EyeEntropyEvaluator` and future block-quality
  decision logic.
- `src/wif/encoders/`: `WaveEncoder`, `LineEncoder`, and `dtcEncoder`
  implementations.
- `src/wif/format/`: WIF container layout, headers, metadata, and binary merge
  logic.
- `src/wif/pipeline/`: full image-to-WIF encode/decode orchestration.
- `src/wif/utils/`: small reusable helpers.
- `src/cli/`: command-line entry points.

## Input Paths

- `data/input/images/`: source PNG or other regular image files.
- `data/input/yuv/`: raw YUV444 test inputs.
- `data/fixtures/`: small fixed files used by tests.

## Output Paths

- `data/output/wif/`: final encoded `.wif` image binaries.
- `data/output/debug/`: intermediate debug artifacts such as generated
  `dtc-blocks-plane` and `wave-blocks-plane` images.
- `data/output/decoded/`: decoded images produced while testing the decoder.

## Test Paths

- `tests/unit/`: tests for individual encoders, evaluators, and helpers.
- `tests/integration/`: full pipeline tests from input image to WIF binary and
  back.

## Documentation Paths

- `docs/specs/`: detailed WIF binary format and algorithm notes.
- `docs/diagrams/`: architecture, pipeline, and data-flow diagrams.

## Tooling Paths

- `tools/`: scripts for inspection, benchmarking, conversion, and debug export.

## Build Paths

- `build/`: native C build output directory. Generated contents are ignored by
  Git, except for `build/.gitkeep` so the folder exists in the repository.

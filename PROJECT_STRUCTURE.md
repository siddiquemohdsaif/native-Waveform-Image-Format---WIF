# Project Structure

Recommended WIF project layout:

```text
native-Waveform-Image-Format---WIF/
  BLUEPRINT.md
  PROJECT_STRUCTURE.md
  src/
    wif/
      encoders/
      evaluators/
      format/
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

- `src/wif/evaluators/`: `EyeEntropyEvaluator` and future block-quality
  decision logic. It routes 16x16 raw plane blocks into DTC and wave planes
  using edge strength.
- `src/wif/encoders/`: `WaveEncoder`, `LineEncoder`, and `dtcEncoder`
  implementations.
- `src/wif/format/`: WIF container layout, headers, metadata, and binary merge
  logic.
- `src/wif/pipeline/`: full image-to-WIF encode/decode orchestration.
- `src/wif/utils/`: small reusable helpers, including image preprocessing to
  YUV444 plane buffers, grayscale debug PNGs, final split-stream RLE+rANS
  route-map compaction, plane reconstruction from compact DTC/Wave planes, and
  YCbCr-to-color PNG export.
- `src/cli/`: command-line entry points, including image preprocessing and raw
  plane evaluation.

## Input Paths

- `data/input/images/`: source PNG or other regular image files.
- `data/input/yuv/`: raw YUV444 test inputs.
- `data/fixtures/`: small fixed files used by tests.

## Output Paths

- `data/output/wif/`: final encoded `.wif` image binaries.
- `data/output/debug/`: intermediate debug artifacts such as generated edge
  maps, compact DTC/Wave block planes, routing bitmaps, and full-size routing
  overlays.
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

- `build/`: native C build output directory. This folder is ignored by Git.

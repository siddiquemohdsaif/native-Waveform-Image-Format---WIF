# WIF Blueprint

WIF stands for **Waveform Image Format**.

WIF is an image format that encodes image data in YUV color planes. If the input
image is PNG or another non-YUV source format, the encoder first converts it into
YUV444 planar data. After conversion, each plane is encoded independently.

## Encoding Pipeline

1. Input image is loaded.
2. If the image is PNG, it is converted to YUV444 planes.
3. The Y, U, and V planes are processed separately.
4. Each plane is split into 16x16 blocks.
5. Every block is passed to `EyeEntropyEvaluator`.
6. `EyeEntropyEvaluator` decides whether the block should be encoded as:
   - DCT block data, using `dtcEncoder`.
   - Wave/line block data, using `WaveEncoder` and `LineEncoder`.
7. Blocks are grouped into two square image planes:
   - `dtc-blocks-plane`
   - `wave-blocks-plane`
8. If either grouped plane has unused area after arranging the blocks into a
   complete square, the remaining area is filled with black blocks.
9. The `dtc-blocks-plane` is encoded with `dtcEncoder`.
10. The `wave-blocks-plane` is encoded with `WaveEncoder` and `LineEncoder`.
11. The DCT binary and wave/line binary are merged into one binary stream for
    the current color plane.
12. The Y, U, and V plane binaries are merged into one final WIF image binary.

## Plane Encoding Model

Each YUV plane uses the same encoding structure:

```text
plane
  -> split into 16x16 blocks
  -> evaluate each block with EyeEntropyEvaluator
  -> route block to DCT or Wave/Line path
  -> pack routed blocks into square block planes
  -> encode square block planes
  -> merge encoded binaries into one plane binary
```

## Full WIF Binary Model

```text
input image
  -> YUV444 conversion
  -> Y plane binary
  -> U plane binary
  -> V plane binary
  -> final WIF binary
```

## Encoder Components

- `EyeEntropyEvaluator`: decides which encoding path is best for each 16x16
  block.
- `dtcEncoder`: encodes blocks selected for DCT-style encoding.
- `WaveEncoder`: encodes blocks selected for waveform encoding.
- `LineEncoder`: encodes line-oriented data used by the waveform path.

## Notes

- WIF uses YUV444 as the normalized internal image representation.
- Each color plane is encoded independently before final binary assembly.
- Block routing is adaptive and happens per 16x16 block.
- Black filler blocks are only used to complete square block planes before
  encoder-specific binary generation.

# transform-vectors

Generates scene-item transform test vectors from real libobs for the
clean-room reimplementation described in
`docs/immersive/scene-editing-behavior-spec.md` (section 9 documents the
file format).

## Build

Configure with the test tree and this target enabled:

```
cmake --preset <your-preset> -DBUILD_TESTS=ON -DENABLE_TRANSFORM_VECTORS=ON
cmake --build --preset <your-preset> --target transform-vectors
```

## Run

```
transform-vectors --out vectors --data-path <repo>/libobs/data
```

`--data-path` must point at the directory holding libobs's effect files when
running from a build tree; without it `obs_reset_video` fails and the tool
falls back to absolute-coordinate, ungrouped cases only.

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `--out DIR` | `.` | Output directory. Writes `transform-vectors-absolute.json` and `transform-vectors-relative.json`. |
| `--mode` | `both` | `absolute`, `relative` or `both` coordinate modes. |
| `--canvas WxH` | `1920x1080` | Base canvas size. Relative mode quantizes against this. |
| `--seed N` | `20261022` | Seed for the pseudo-random sweeps. Pin it in the consuming test suite. |
| `--random N` | `3000` | Number of random single-item cases per mode. |
| `--grouped N` | `300` | Number of grouped cases per mode. |

The exhaustive core (every alignment, bounds type and bounds alignment with
three scales and both crop-to-bounds values) is always emitted.

Each run records the libobs version and platform in the `generator` block.
Flipped items with non-left alignment and no bounds can differ between
x86-64 and arm64 builds (see spec section 3.5); generate on the platform
that will run OBS in production.

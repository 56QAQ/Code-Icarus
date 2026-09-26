# Local Vulkan black surfaces (Godot 4.5.1)

## Reproduction and cause

Reproduced on Windows with an NVIDIA GeForce RTX 5090, Vulkan 1.4.329,
Forward+, both in the user's exported game and the repository's main scene.
The default seed-1 village view at 1280×720 contained about 525,560 almost-black
pixels (57% of the frame), despite nonzero ambient light.

The fault was isolated to **sky radiance filtering**, rather than the camera,
voxel textures, normals, shadow maps, or SSAO. Controlled captures of the same
paused scene showed:

| Change from the original scene | Almost-black pixels |
| --- | ---: |
| Disable directional shadows | ~525,570 |
| Disable SSAO | ~525,570 |
| Disable the sun | ~525,570 |
| Replace voxel shaders with a constant-colour lit shader | ~525,560 |
| Disable aerial-perspective fog | ~100,260 |
| Disable aerial-perspective fog and sky reflections | 0 |
| Set `Sky.process_mode` to `PROCESS_MODE_REALTIME` | 0 |
| Set it back to `PROCESS_MODE_QUALITY` or `PROCESS_MODE_INCREMENTAL` | ~525,570 |

Even a constant-colour sky reproduced the fault with the quality filter. This
localizes it to the engine/GPU radiance processing path, not the procedural sky's
colour calculation. The exact driver/shader arithmetic defect has not been
established. The corrupted radiance is consumed both by material reflections
(black faces) and aerial fog (large view-dependent black regions).

Godot's [Sky documentation](https://docs.godotengine.org/en/4.5/classes/class_sky.html)
states that automatic mode selects incremental processing for skies using light
variables or custom uniforms. Quality and incremental processing share the
importance-sampling filter; realtime uses a different, fast filter intended for
frequently changing skies. Cloud testing with software Vulkan cannot establish
whether this hardware-specific path works on the player's GPU.

## Fix

`game/scripts/main.gd` explicitly selects `Sky.PROCESS_MODE_REALTIME` and the
required 256×256 radiance size. This matches the existing per-frame daylight and
weather updates. It retains Forward+, ambient lighting, reflections, SSAO,
shadows, and fog. No simulation or material changes are needed.

Realtime filtering trades some reflection precision for faster updates, which is
appropriate for this changing, smooth procedural sky. The existing OpenGL option
remains available, but is not needed for this fix on the tested GPU.

## Regression check

Run on a real rendering device with a display, not `--headless`:

```sh
godot --path game --resolution 1280x720 --script res://tests/render_test.gd -- --seed 1 --hide-ui --render-test-out /absolute/output/directory
```

The test renders the actual village, reverse angle, overhead view and underside,
checks the sky mode, saves optional PNGs, and fails if near-zero pixels cover
0.5% or more of a frame. All four views had zero such pixels on the RTX 5090
after the fix. Unlike a headless smoke test, this checks rendered output.

Validation also passed for all four views with OpenGL Compatibility, and with
simulation ticks advancing between Vulkan frames (changing daylight). The headless
kernel/GDExtension smoke test passed. A fresh Windows release export was launched
on the same GPU and visually checked at 14:00 from the opposite side of the village.
The exported executable and extension DLL matched the original game's SHA-256
hashes; the resource pack's only code change was `scripts/main.gdc` (other differences
were text line endings and the resource UID cache).

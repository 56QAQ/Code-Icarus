// Hash-based gradient noise. Pure functions of (seed, coordinates) so that
// world generation is independent of the order in which cells are generated.
#pragma once

#include "icarus/util/rng.h"

namespace icarus {

float gradient_noise2(u64 seed, float x, float y);            // ~[-1, 1]
float gradient_noise3(u64 seed, float x, float y, float z);   // ~[-1, 1]
float fbm2(u64 seed, float x, float y, int octaves, float lacunarity = 2.0f, float gain = 0.5f);
float fbm3(u64 seed, float x, float y, float z, int octaves, float lacunarity = 2.0f, float gain = 0.5f);
// Ridged variant, output ~[0, 1]
float ridged2(u64 seed, float x, float y, int octaves);

}  // namespace icarus

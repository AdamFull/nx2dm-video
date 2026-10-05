#pragma once

// What the video draw is handed, shared by C++ and Slang as nx_interop.h
// describes; video.slang includes it.

#include "rendering/rhi/shaders/nx_interop.h"

/// Vectors first, then scalars: that order is 8-aligned alike under std430
/// and in C++, so neither side needs padding to guess.
struct GpuVideoPush {
  float4 rect = float4(-1.f, -1.f, 1.f, 1.f);
  float2 uv_scale = float2(1.f, 1.f);
  /// Kr and Kb of the source's colour matrix; BT.709 until told otherwise.
  float2 luma_weights = float2(0.2126f, 0.0722f);
  NxTexture2D<float4> luma = {};
  NxTexture2D<float4> chroma = {};
  uint full_range = 0u;
  uint _pad0 = 0u;
};
NX_SHARED_SIZE(GpuVideoPush, 48);

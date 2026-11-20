/**
 * @file    gpu/render/render_scale.h
 * @brief   Render resolution: host pixels per guest pixel.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace renut::gpu {

// Host pixels per guest pixel of a render target, and of what resolves from
// it, as one of a fixed set of ratios: the render resolutions
// (renut_resolution: 1x, 480p, 1080p, 1440p, 4K) and the targets that render
// finer than the rest (graphics_options.h: shadow maps at 4x, ambient
// occlusion at twice the resolution: 4/3, 6x). Code 0 is 1x, the scale of all
// content that is not rendered (guest textures). Shaders get the code in the
// descriptor index (kResolutionScaleShift) and have the same factors
// (renutTextureScale in shader_common.h).
// fnxf_xiii (2026-11-07, dynamic resolution): the fraction of a dynamic-resolution target's host size drawn this frame
// (render/dynamic_resolution.cpp, set at the frame's start; 1 without it).
extern float g_drs_factor;

struct RenderScale {
  // fnxf_xiii (2026-11-08): 6/5, 15/8 and 5/4 (codes 8-10): a 1600x900 / 1024x576 scene's post-processing at a 1920x1080
  // output, a 1024x576 one's at 1280x720 (FSR 3.1 before it, graphics_options.cpp). Deko3d only: a descriptor index carries 3 bits of the code
  // (kResolutionScaleShift, masked), which only the Vulkan shaders read (deko3d's: pass scale1, no scale divisor).
  static constexpr uint32_t kCount = 11;
  static constexpr uint32_t kNumerators[kCount] = {1, 2, 3, 2, 3, 4, 6, 4, 6, 15, 5};
  static constexpr uint32_t kDenominators[kCount] = {1, 3, 2, 1, 1, 1, 1, 3, 5, 8, 4};

  uint32_t code = 0;
  // fnxf_xiii (2026-11-07, dynamic resolution): a scene target whose host texture keeps its size ('code' times the guest
  // one) while only g_drs_factor of it is drawn from its top-left corner: viewports, scissors, clears and resolves are
  // scaled by it (Coord, DrawFactor), and the shaders that sample what is resolved from it scale their texture
  // coordinates by it too (the descriptor index's kDrsTextureBit, SharedConstants::drs_fill).
  bool drs = false;
  // fnxf_xiii (2026-11-19, fnxf_xiii_drs_sized): a resolve destination of a dynamic-resolution target made at the size
  // the target was drawn at: the fraction of 'code' times the guest size, times 4096 (0: none, the whole size). Its
  // host texture holds just the drawn part, so the shaders sample it as any texture (no dynamic-resolution code in
  // them); each level's texture is kept aside for when the level comes back (textures.cpp ResolveTarget's spares).
  uint16_t sized = 0;

  // Host pixels per guest pixel of the host texture (its size).
  float Factor() const { return float(kNumerators[code]) / float(kDenominators[code]); }
  // fnxf_xiii: host pixels per guest pixel as drawn this frame.
  float DrawFactor() const {
    return drs ? Factor() * g_drs_factor : sized ? Factor() * float(sized) / 4096.0f : Factor();
  }
  bool operator==(const RenderScale &) const = default;
  // fnxf_xiii: code, drs and sized in one number (keys).
  uint32_t Key() const {  // fnxf_xiii (2026-11-08): 16 (codes up to 10)
    return code | (drs ? 16u : 0u) | (uint32_t(sized) << 5);
  }

  // A guest pixel edge in host pixels as drawn, to the nearest: rects that share an edge in guest pixels share it in
  // host pixels.
  int32_t Coord(int32_t guest) const {
    const double host = double(guest) * kNumerators[code] / kDenominators[code];
    return int32_t(std::lround(drs ? host * double(g_drs_factor) : sized ? host * double(sized) / 4096.0 : host));
  }

  // A guest size in host pixels of the host texture (dynamic resolution aside), 1 at least.
  uint32_t Size(uint32_t guest) const {
    const double host = double(guest) * kNumerators[code] / kDenominators[code];
    return std::max<uint32_t>(uint32_t(std::lround(sized ? host * double(sized) / 4096.0 : host)), 1u);
  }
  // fnxf_xiii: a guest size in host pixels as drawn this frame, 1 at least.
  uint32_t DrawnSize(uint32_t guest) const { return std::max<uint32_t>(uint32_t(Coord(int32_t(guest))), 1u); }

  // The scale of ratio numerator / denominator, if it is one of them.
  static bool FromRatio(uint32_t numerator, uint32_t denominator, RenderScale &out) {
    for (uint32_t c = 0; c < kCount; ++c) {
      if (uint64_t(numerator) * kDenominators[c] == uint64_t(kNumerators[c]) * denominator) {
        out.code = c;
        return true;
      }
    }
    return false;
  }

  // This scale times 'multiplier', or this one when that is not among them.
  RenderScale Times(uint32_t multiplier) const {
    RenderScale out = *this;
    FromRatio(kNumerators[code] * multiplier, kDenominators[code], out);
    return out;
  }
};

} // namespace renut::gpu

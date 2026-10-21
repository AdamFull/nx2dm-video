#if defined(NX_RHI_VULKAN)

#include "nx/gfx/video/vp9.h"

namespace nx::gfx::video {
namespace {

constexpr u32 VP9_FRAME_MARKER = 2;
constexpr u32 VP9_SYNC_CODE = 0x498342;
constexpr u32 CS_RGB = 7;
constexpr u32 MAX_TILE_WIDTH_B64 = 64;
constexpr u32 MIN_TILE_WIDTH_B64 = 4;

// A big-endian bit reader over the frame bytes. Reads past the end return zero
// and raise `overrun`, so a truncated header fails cleanly rather than walking
// off the buffer.
class BitReader {
public:
  BitReader(const u8 *data, const u32 size) noexcept
      : m_data(data), m_size(size) {}

  u32 bit() noexcept {
    const u32 byte = m_pos >> 3;
    if (byte >= m_size) {
      m_overrun = true;
      return 0;
    }
    const u32 shift = 7u - (m_pos & 7u);
    ++m_pos;
    return (m_data[byte] >> shift) & 1u;
  }

  u32 f(const u32 n) noexcept {
    u32 v = 0;
    for (u32 i = 0; i < n; ++i)
      v = (v << 1) | bit();
    return v;
  }

  // s(n): magnitude then a sign bit.
  i32 s(const u32 n) noexcept {
    const i32 value = nx::cast<i32>(f(n));
    return bit() ? -value : value;
  }

  // A probability: present flag, then eight bits, else the neutral 255.
  u8 prob() noexcept { return bit() ? nx::cast<u8>(f(8)) : nx::cast<u8>(255); }

  i8 delta_q() noexcept { return bit() ? nx::cast<i8>(s(4)) : nx::cast<i8>(0); }

  [[nodiscard]] u32 byte_pos_ceil() const noexcept { return (m_pos + 7u) >> 3; }
  [[nodiscard]] bool overrun() const noexcept { return m_overrun; }

private:
  const u8 *m_data;
  u32 m_size;
  u32 m_pos = 0;
  bool m_overrun = false;
};

void color_config(BitReader &r, const u32 profile, vp9_frame_header &out) {
  u8 bit_depth = 8;
  if (profile >= 2)
    bit_depth = r.bit() ? 12 : 10;
  const u32 cs = r.f(3);

  u8 subsampling_x = 1;
  u8 subsampling_y = 1;
  u32 color_range = 0;
  if (cs != CS_RGB) {
    color_range = r.bit();
    if (profile == 1 || profile == 3) {
      subsampling_x = nx::cast<u8>(r.bit());
      subsampling_y = nx::cast<u8>(r.bit());
      r.bit();
    }
  } else {
    color_range = 1;
    subsampling_x = 0;
    subsampling_y = 0;
    if (profile == 1 || profile == 3)
      r.bit();
  }

  out.color.BitDepth = bit_depth;
  out.color.subsampling_x = subsampling_x;
  out.color.subsampling_y = subsampling_y;
  out.color.color_space = nx::cast<StdVideoVP9ColorSpace>(cs);
  out.color.flags.color_range = color_range;
}

void loop_filter_params(BitReader &r, vp9_frame_header &out) {
  StdVideoVP9LoopFilter &lf = out.loop_filter;
  lf.loop_filter_level = nx::cast<u8>(r.f(6));
  lf.loop_filter_sharpness = nx::cast<u8>(r.f(3));
  const u32 delta_enabled = r.bit();
  lf.flags.loop_filter_delta_enabled = delta_enabled;
  if (delta_enabled) {
    const u32 delta_update = r.bit();
    lf.flags.loop_filter_delta_update = delta_update;
    if (delta_update) {
      for (u32 i = 0; i < STD_VIDEO_VP9_MAX_REF_FRAMES; ++i)
        if (r.bit()) {
          lf.update_ref_delta |= nx::cast<u8>(1u << i);
          lf.loop_filter_ref_deltas[i] = nx::cast<i8>(r.s(6));
        }
      for (u32 i = 0; i < STD_VIDEO_VP9_LOOP_FILTER_ADJUSTMENTS; ++i)
        if (r.bit()) {
          lf.update_mode_delta |= nx::cast<u8>(1u << i);
          lf.loop_filter_mode_deltas[i] = nx::cast<i8>(r.s(6));
        }
    }
  }
}

void quantization_params(BitReader &r, vp9_frame_header &out) {
  out.picture.base_q_idx = nx::cast<u8>(r.f(8));
  out.picture.delta_q_y_dc = r.delta_q();
  out.picture.delta_q_uv_dc = r.delta_q();
  out.picture.delta_q_uv_ac = r.delta_q();
}

void segmentation_params(BitReader &r, vp9_frame_header &out) {
  // Per SEG_LVL feature: how many bits it carries, and whether it is signed.
  static constexpr u8 feature_bits[STD_VIDEO_VP9_SEG_LVL_MAX] = {8, 6, 2, 0};
  static constexpr bool feature_signed[STD_VIDEO_VP9_SEG_LVL_MAX] = {
      true, true, false, false};

  StdVideoVP9Segmentation &seg = out.segmentation;
  const u32 enabled = r.bit();
  out.picture.flags.segmentation_enabled = enabled;
  if (!enabled)
    return;

  const u32 update_map = r.bit();
  seg.flags.segmentation_update_map = update_map;
  if (update_map) {
    for (u32 i = 0; i < STD_VIDEO_VP9_MAX_SEGMENTATION_TREE_PROBS; ++i)
      seg.segmentation_tree_probs[i] = r.prob();
    const u32 temporal_update = r.bit();
    seg.flags.segmentation_temporal_update = temporal_update;
    for (u32 i = 0; i < STD_VIDEO_VP9_MAX_SEGMENTATION_PRED_PROB; ++i)
      seg.segmentation_pred_prob[i] =
          temporal_update ? r.prob() : nx::cast<u8>(255);
  }

  const u32 update_data = r.bit();
  seg.flags.segmentation_update_data = update_data;
  if (update_data) {
    seg.flags.segmentation_abs_or_delta_update = r.bit();
    for (u32 i = 0; i < STD_VIDEO_VP9_MAX_SEGMENTS; ++i)
      for (u32 j = 0; j < STD_VIDEO_VP9_SEG_LVL_MAX; ++j) {
        i16 value = 0;
        if (r.bit()) {
          seg.FeatureEnabled[i] |= nx::cast<u8>(1u << j);
          value = nx::cast<i16>(r.f(feature_bits[j]));
          if (feature_signed[j] && r.bit())
            value = nx::cast<i16>(-value);
        }
        seg.FeatureData[i][j] = value;
      }
  }
}

void tile_info(BitReader &r, const u32 frame_width, vp9_frame_header &out) {
  const u32 mi_cols = (frame_width + 7u) >> 3;
  const u32 sb64_cols = (mi_cols + 7u) >> 3;

  u32 min_log2 = 0;
  while ((MAX_TILE_WIDTH_B64 << min_log2) < sb64_cols)
    ++min_log2;
  u32 max_log2 = 1;
  while ((sb64_cols >> max_log2) >= MIN_TILE_WIDTH_B64)
    ++max_log2;
  --max_log2;

  u32 cols_log2 = min_log2;
  while (cols_log2 < max_log2) {
    if (r.bit())
      ++cols_log2;
    else
      break;
  }
  u32 rows_log2 = r.bit();
  if (rows_log2)
    rows_log2 += r.bit();

  out.picture.tile_cols_log2 = nx::cast<u8>(cols_log2);
  out.picture.tile_rows_log2 = nx::cast<u8>(rows_log2);
}

StdVideoVP9InterpolationFilter read_interpolation_filter(BitReader &r) {
  static constexpr StdVideoVP9InterpolationFilter literal[4] = {
      STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP_SMOOTH,
      STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP,
      STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP_SHARP,
      STD_VIDEO_VP9_INTERPOLATION_FILTER_BILINEAR};
  if (r.bit())
    return STD_VIDEO_VP9_INTERPOLATION_FILTER_SWITCHABLE;
  return literal[r.f(2)];
}

}

bool parse_vp9_frame_header(const u8 *const data, const u32 size,
                            const u32 ref_width[8], const u32 ref_height[8],
                            vp9_frame_header &out) {
  out = vp9_frame_header{};
  if (data == nullptr || size == 0)
    return false;

  BitReader r(data, size);
  if (r.f(2) != VP9_FRAME_MARKER)
    return false;

  const u32 profile_low = r.bit();
  const u32 profile_high = r.bit();
  const u32 profile = (profile_high << 1) | profile_low;
  if (profile == 3)
    r.bit();
  out.picture.profile = nx::cast<StdVideoVP9Profile>(profile);

  if (r.bit()) {
    out.show_existing_frame = true;
    out.frame_to_show_map_idx = nx::cast<u8>(r.f(3));
    out.valid = !r.overrun();
    return out.valid;
  }

  const u32 frame_type = r.bit();
  const u32 show_frame = r.bit();
  const u32 error_resilient = r.bit();
  out.picture.frame_type = frame_type == 0 ? STD_VIDEO_VP9_FRAME_TYPE_KEY
                                           : STD_VIDEO_VP9_FRAME_TYPE_NON_KEY;
  out.picture.flags.show_frame = show_frame;
  out.picture.flags.error_resilient_mode = error_resilient;

  u32 frame_width = 0;
  u32 frame_height = 0;
  bool frame_is_intra = false;

  const auto frame_size = [&] {
    frame_width = r.f(16) + 1;
    frame_height = r.f(16) + 1;
  };
  const auto render_size = [&] {
    if (r.bit()) {
      r.f(16);
      r.f(16);
    }
  };

  if (frame_type == 0) {
    if (r.f(24) != VP9_SYNC_CODE)
      return false;
    color_config(r, profile, out);
    frame_size();
    render_size();
    out.picture.refresh_frame_flags = 0xFF;
    frame_is_intra = true;
  } else {
    const u32 intra_only = show_frame ? 0u : r.bit();
    out.picture.flags.intra_only = intra_only;
    frame_is_intra = intra_only != 0;

    if (error_resilient == 0)
      out.picture.reset_frame_context = nx::cast<u8>(r.f(2));

    if (intra_only) {
      if (r.f(24) != VP9_SYNC_CODE)
        return false;
      if (profile > 0) {
        color_config(r, profile, out);
      } else {
        out.color.BitDepth = 8;
        out.color.subsampling_x = 1;
        out.color.subsampling_y = 1;
        out.color.color_space = STD_VIDEO_VP9_COLOR_SPACE_BT_601;
      }
      out.picture.refresh_frame_flags = nx::cast<u8>(r.f(8));
      frame_size();
      render_size();
    } else {
      out.picture.refresh_frame_flags = nx::cast<u8>(r.f(8));
      u32 sign_bias_mask = 0;
      bool found_ref = false;
      for (u32 i = 0; i < STD_VIDEO_VP9_REFS_PER_FRAME; ++i) {
        out.ref_frame_idx[i] = nx::cast<u8>(r.f(3));
        if (r.bit())
          sign_bias_mask |= 1u << (i + 1);
      }
      out.picture.ref_frame_sign_bias_mask = nx::cast<u8>(sign_bias_mask);

      for (u32 i = 0; i < STD_VIDEO_VP9_REFS_PER_FRAME; ++i) {
        if (r.bit()) {
          const u32 slot = out.ref_frame_idx[i];
          if (ref_width != nullptr && slot < 8) {
            frame_width = ref_width[slot];
            frame_height = ref_height[slot];
          }
          found_ref = true;
          break;
        }
      }
      if (!found_ref)
        frame_size();
      render_size();

      out.picture.flags.allow_high_precision_mv = r.bit();
      out.picture.interpolation_filter = read_interpolation_filter(r);
    }
  }

  if (error_resilient == 0) {
    out.picture.flags.refresh_frame_context = r.bit();
    out.picture.flags.frame_parallel_decoding_mode = r.bit();
  } else {
    out.picture.flags.frame_parallel_decoding_mode = 1;
  }

  out.picture.frame_context_idx = nx::cast<u8>(r.f(2));
  if (frame_is_intra || error_resilient)
    out.picture.frame_context_idx = 0;

  loop_filter_params(r, out);
  quantization_params(r, out);
  segmentation_params(r, out);
  tile_info(r, frame_width, out);

  out.compressed_header_size = r.f(16);
  out.uncompressed_header_size = r.byte_pos_ceil();
  out.frame_width = frame_width;
  out.frame_height = frame_height;

  out.valid = !r.overrun() && out.compressed_header_size > 0 &&
              out.uncompressed_header_size + out.compressed_header_size <= size;
  return out.valid;
}

} // namespace nx::gfx::video

#endif

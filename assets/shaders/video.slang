// A decoded video frame is three planes of Y'CbCr (4:2:0), not RGB. This
// samples the three and converts, drawing one quad into a screen rectangle
// given in NDC. The quad is generated from the vertex id, so there is no vertex
// buffer - a full-screen clip is just the rectangle (-1,-1)..(1,1).

import bindless;

struct VideoPush {
    /// Destination rectangle in NDC as (x0, y0, x1, y1). (-1,-1,1,1) is
    /// full-screen.
    float4 rect;

    uint y_plane;
    uint cb_plane;
    uint cr_plane;
    uint sampler_index;
    float2 uv_scale;
    float2 luma;      ///< (kr, kb): the matrix's luma weights.
    uint full_range;  ///< 0 = limited (broadcast) range, 1 = full.
};

[[vk::push_constant]]
VideoPush pc;

float3 ycbcr_to_rgb(float y, float cb, float cr) {
    const float kr = pc.luma.x;
    const float kb = pc.luma.y;
    const float kg = 1.0 - kr - kb;

    float yl, b, r;
    if (pc.full_range != 0) {
        yl = y;
        b  = cb - 128.0 / 255.0;
        r  = cr - 128.0 / 255.0;
    } else {
        yl = (y  - 16.0 / 255.0) * (255.0 / 219.0);
        b  = (cb - 128.0 / 255.0) * (255.0 / 224.0);
        r  = (cr - 128.0 / 255.0) * (255.0 / 224.0);
    }

    float3 rgb;
    rgb.r = yl + 2.0 * (1.0 - kr) * r;
    rgb.b = yl + 2.0 * (1.0 - kb) * b;
    rgb.g = yl - (2.0 * (1.0 - kr) * kr / kg) * r
               - (2.0 * (1.0 - kb) * kb / kg) * b;
    return saturate(rgb);
}

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vs_main(uint vertex_id: SV_VertexID) {
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0),
    };
    const float2 corner = corners[vertex_id];
    VertexOutput output;
    output.position = float4(lerp(pc.rect.xy, pc.rect.zw, corner), 0.0, 1.0);
    // V is flipped: every set_viewport in this engine emits a negative-height
    // Vulkan viewport, so NDC +y is the top of the frame. Without this the row
    // 0 of the image samples at the bottom and the video is upside down.
    output.uv = float2(corner.x, 1.0 - corner.y);
    return output;
}

[shader("fragment")]
float4 fs_main_nv12(VertexOutput input): SV_Target {
    const float2 uv = input.uv * pc.uv_scale;
    const float  y  = sample_texture(pc.y_plane,  pc.sampler_index, uv).r;
    const float2 c  = sample_texture(pc.cb_plane, pc.sampler_index, uv).rg;
    return float4(ycbcr_to_rgb(y, c.r, c.g), 1.0);
}

[shader("fragment")]
float4 fs_main(VertexOutput input): SV_Target {
    const float y  = sample_texture(pc.y_plane,  pc.sampler_index, input.uv).r;
    const float cb = sample_texture(pc.cb_plane, pc.sampler_index, input.uv).r;
    const float cr = sample_texture(pc.cr_plane, pc.sampler_index, input.uv).r;
    return float4(ycbcr_to_rgb(y, cb, cr), 1.0);
}

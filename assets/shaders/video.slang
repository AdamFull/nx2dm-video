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
};

[[vk::push_constant]]
VideoPush pc;

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
float4 fs_main(VertexOutput input): SV_Target {
    const float y  = sample_texture(pc.y_plane,  pc.sampler_index, input.uv).r;
    const float cb = sample_texture(pc.cb_plane, pc.sampler_index, input.uv).r;
    const float cr = sample_texture(pc.cr_plane, pc.sampler_index, input.uv).r;

    const float yl = (y  - 16.0 / 255.0) * (255.0 / 219.0);
    const float b  = (cb - 128.0 / 255.0) * (255.0 / 224.0);
    const float r  = (cr - 128.0 / 255.0) * (255.0 / 224.0);

    float3 rgb;
    rgb.r = yl + 1.5748 * r;
    rgb.g = yl - 0.1873 * b - 0.4681 * r;
    rgb.b = yl + 1.8556 * b;
    return float4(saturate(rgb), 1.0);
}

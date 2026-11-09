#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
// (hookshot) NVIDIA DLSS's actor motion (dlss_actor.vert): where the draw is what the scene shows (its depth within
// pc.tolerance steps of the scene's), the motion from where it is now (the pixel, the jitter taken off) to where it was, in
// render pixels as shaders/dlss_prep.comp makes them
layout(std430, binding = 0) readonly buffer Data { uint w[]; } data;
layout(binding = 1) uniform sampler2D scene_depth;
layout(push_constant) uniform P {
    uint64_t vertices;   // (the vertex stream's address)
    uint64_t indices;    // (the index buffer's address, 4-aligned down; first: the draw's first index in it)
    uint64_t origins[2]; // (the draw's origin, xyz: this frame's and the frame before's)
    uint64_t bones[2];   // (its palettes: this frame's, the frame before's)
    uint nbones;
    uint stride;    // (in words)
    uint count;     // (vertices in the stream: an index past them draws nothing)
    uint first;
    uint itype;     // (0: 8-bit, 1: 16-bit, 2: 32-bit indices)
    int base_vertex;
    uint pos, sets;          // (byte offsets in a vertex, the joint and weight sets: 1 or 2)
    uint joints[2], weights[2];
    uint formats;            // (1: the position RGBA16F, else RGB32F; 2 << set: that set's joints RGBA8UI, else RGBA16UI)
    uint tolerance;          // (the fragment stage's)
} pc;
layout(location = 0) in vec4 before;
layout(location = 0) out vec2 motion;

void main() {
    const uint scene = floatBitsToUint(texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r);
    const uint here = floatBitsToUint(gl_FragCoord.z);
    if (max(scene, here) - min(scene, here) > pc.tolerance || before.w <= 1e-6) discard;
    const vec2 size = vec2(uintBitsToFloat(data.w[34]), uintBitsToFloat(data.w[35]));
    const vec2 jitter = vec2(uintBitsToFloat(data.w[32]), uintBitsToFloat(data.w[33]));
    const vec2 now = vec2(gl_FragCoord.x / size.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / size.y * 2.0) - jitter;
    const vec2 then = before.xy / before.w;
    const vec2 px = vec2(then.x - now.x, now.y - then.y) * 0.5 * size;
    if (!all(lessThan(abs(px), vec2(16384.0)))) discard;
    motion = px;
}

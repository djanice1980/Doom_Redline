// The quad fragment shader: quad.frag for the world pass, quad_hud.frag (HUD_PASS) for
// the HUD pass, which has one colour output where the world pass has two.
#include "common.glsl"
#include "atlas_frag.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vLight;
layout(location = 3) in vec3 vWorldPos;
layout(location = 4) in float vMode;
layout(location = 5) flat in float vFlags;   // flat: decoded with int(), see cube.vert

layout(location = 0) out vec4 outColor;
#ifndef HUD_PASS
layout(location = 1) out vec4 outRefl;   // covers the floor's reflection as much as it covers the floor (see cube_frag_body.glsl)
#endif

void main() {
    vec4 tex = sampleAtlas(vUV);
    float a = tex.a * vColor.a;
    bool world = vMode < 0.5 || vMode > 1.5;
    bool soft = (int(vFlags) & 4) != 0;
    if (world && !soft && tex.a < 0.5) discard;   // hard-edged Doom sprites and decals
    if (soft && a < 0.01) discard;
    // HDR screen quads (the weapon) write depth so the fog pass leaves them alone: only
    // where the sprite actually is.
    if (!world && !soft && (int(vFlags) & 2) != 0 && tex.a < 0.5) discard;
    vec3 col = tex.rgb * vColor.rgb * vLight;
    if (world) col = applyFog(col, vWorldPos);
    outColor = vec4(col, a);
#ifndef HUD_PASS
    outRefl = vec4(0.0, 0.0, 0.0, a);
#endif
}

#version 450
#include "common.glsl"
layout(location = 0) in vec4 iPos;
layout(location = 1) in vec4 iSize;
layout(location = 2) in vec4 iUVRect;
layout(location = 3) in vec4 iColor;
layout(location = 4) in vec4 iParams;
layout(location = 0) out vec2 vUV;
layout(location = 1) out float vAlpha;
const vec2 corners[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(1,1), vec2(0,0), vec2(1,1), vec2(0,1));
void main() {
    vec2 c = corners[gl_VertexIndex];
    vec2 local = (c - iSize.zw) * iSize.xy;
    vec2 uvc = c;
    if (iParams.z > 0.5) uvc.x = 1.0 - uvc.x;
    vUV = vec2(mix(iUVRect.x, iUVRect.z, uvc.x), mix(iUVRect.w, iUVRect.y, uvc.y));
    vAlpha = iColor.a;
    // Decals (modes 2/3) lie on surfaces and cast nothing: throw them out of the clip volume.
    if (iParams.x > 1.5) { gl_Position = vec4(4.0, 4.0, 4.0, 1.0); return; }
    // Same camera-facing card as the main pass so the shadow matches what is drawn.
    vec3 camRight = vec3(u.view[0][0], u.view[1][0], u.view[2][0]);
    vec3 right = normalize(vec3(camRight.x, 0.0, camRight.z));
    vec3 world = iPos.xyz + right * local.x + vec3(0.0, 1.0, 0.0) * local.y;
    gl_Position = u.lightViewProj * vec4(world, 1.0);
}

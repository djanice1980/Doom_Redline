#version 450
#include "common.glsl"

// Per-instance only (binding 0). The quad corners come from gl_VertexIndex.
layout(location = 0) in vec4 iPos;      // world xyz (mode 0) or screen xy in pixels (mode 1); w = rotation (radians, screen mode)
layout(location = 1) in vec4 iSize;     // x width, y height, z anchorX, w anchorY (0..1 inside the quad; (0.5,0) = bottom centre)
layout(location = 2) in vec4 iUVRect;   // u0 v0 u1 v1
layout(location = 3) in vec4 iColor;    // rgba tint
layout(location = 4) in vec4 iParams;   // x mode (0 world billboard, 1 screen), y lit (0/1), z flip x (0/1), w flags

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vLight;   // precomputed lighting multiplier for lit billboards
layout(location = 3) out vec3 vWorldPos;
layout(location = 4) out float vMode;

const vec2 corners[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(1,1), vec2(0,0), vec2(1,1), vec2(0,1));

void main() {
    vec2 c = corners[gl_VertexIndex];
    vec2 local = (c - iSize.zw) * iSize.xy;   // metres (world) or pixels (screen)
    vec2 uvc = c;
    if (iParams.z > 0.5) uvc.x = 1.0 - uvc.x;
    // Atlas rows run top-down while local y runs bottom-up.
    vUV = vec2(mix(iUVRect.x, iUVRect.z, uvc.x), mix(iUVRect.w, iUVRect.y, uvc.y));
    vColor = iColor;
    vMode = iParams.x;
    vLight = vec3(1.0);

    if (iParams.x < 0.5) {
        // World billboard: face the camera around the vertical axis only (Doom style).
        vec3 camRight = vec3(u.view[0][0], u.view[1][0], u.view[2][0]);
        vec3 right = normalize(vec3(camRight.x, 0.0, camRight.z));
        vec3 up = vec3(0.0, 1.0, 0.0);
        vec3 world = iPos.xyz + right * local.x + up * local.y;
        vWorldPos = world;
        if (iParams.y > 0.5) {
            vec3 toCam = normalize(u.cameraPos.xyz - iPos.xyz);
            vLight = shade(iPos.xyz + vec3(0.0, 0.5 * iSize.y, 0.0), toCam, vec3(1.0), 0.9, 0.0);
        }
        gl_Position = u.viewProj * vec4(world, 1.0);
    } else {
        float r = iPos.w;
        vec2 rot = vec2(local.x * cos(r) - local.y * sin(r), local.x * sin(r) + local.y * cos(r));
        // Screen space: origin top-left in pixels, y down. local.y is "up" so flip.
        vec2 px = iPos.xy + vec2(rot.x, -rot.y);
        vec2 ndc = vec2(px.x * u.screen.z * 2.0 - 1.0, 1.0 - px.y * u.screen.w * 2.0);   // top-left origin, +Y-up viewport
        vWorldPos = vec3(0.0);
        gl_Position = vec4(ndc, 0.0, 1.0);
    }
}

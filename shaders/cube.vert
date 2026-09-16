#version 450
#include "common.glsl"

// Per-vertex (binding 0)
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 9) in vec3 inTangent;
// Per-instance (binding 1)
layout(location = 3) in vec4 iPosScale;   // xyz centre, w uniform scale
layout(location = 4) in vec4 iColor;      // rgba tint
layout(location = 5) in vec4 iEmissive;   // rgb, w strength
layout(location = 6) in vec4 iUVRect;     // u0 v0 u1 v1
layout(location = 7) in vec4 iParams;     // x roughness, y metallic, z anim phase, w flags
layout(location = 8) in vec4 iRot;        // x rotation about X, y about Y (radians)

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec4 vColor;
layout(location = 4) out vec4 vEmissive;
layout(location = 5) out vec4 vParams;
layout(location = 6) out vec3 vTangent;
layout(location = 7) out vec2 vFaceUV;   // 0..1 across the face: material maps tile once per cube

void main() {
    float s = iPosScale.w;
    // flag bit 1: pulse (used for the "refusing" red blocks)
    float pulse = 0.0;
    if ((int(iParams.w) & 1) != 0) pulse = 0.04 * sin(u.cameraPos.w * 6.0 + iParams.z);
    // Rotate about Y then X (rigid board tilt).
    float cy = cos(iRot.y), sy = sin(iRot.y);
    float cx = cos(iRot.x), sx = sin(iRot.x);
    mat3 ry = mat3(cy, 0.0, -sy,  0.0, 1.0, 0.0,  sy, 0.0, cy);
    mat3 rx = mat3(1.0, 0.0, 0.0,  0.0, cx, sx,  0.0, -sx, cx);
    mat3 R = rx * ry;
    vec3 world = iPosScale.xyz + R * (inPos * (s * (1.0 + pulse)));
    vWorldPos = world;
    vNormal = R * inNormal;
    vTangent = R * inTangent;
    vFaceUV = inUV;
    vUV = mix(iUVRect.xy, iUVRect.zw, inUV);
    vColor = iColor;
    vEmissive = iEmissive;
    vParams = iParams;
    gl_Position = u.viewProj * vec4(world, 1.0);
}

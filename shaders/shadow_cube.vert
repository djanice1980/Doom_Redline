#version 450
#include "common.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 iPosScale;
layout(location = 4) in vec4 iColor;
layout(location = 5) in vec4 iEmissive;
layout(location = 6) in vec4 iUVRect;
layout(location = 7) in vec4 iParams;
layout(location = 8) in vec4 iRot;
void main() {
    float s = iPosScale.w;
    float pulse = 0.0;
    if ((int(iParams.w) & 1) != 0) pulse = 0.04 * sin(u.cameraPos.w * 6.0 + iParams.z);
    float cy = cos(iRot.y), sy = sin(iRot.y);
    float cx = cos(iRot.x), sx = sin(iRot.x);
    mat3 ry = mat3(cy, 0.0, -sy,  0.0, 1.0, 0.0,  sy, 0.0, cy);
    mat3 rx = mat3(1.0, 0.0, 0.0,  0.0, cx, sx,  0.0, -sx, cx);
    vec3 world = iPosScale.xyz + (rx * ry) * (inPos * (s * (1.0 + pulse)));
    gl_Position = u.lightViewProj * vec4(world, 1.0);
}

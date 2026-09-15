#version 450
#include "common.glsl"

// Depth-only pass of a voxel mesh into the sun's shadow map.
layout(location = 0) in ivec4 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform PC {
    mat4 model;
    vec4 color;
    vec4 emissive;
} pc;

void main() {
    gl_Position = u.lightViewProj * pc.model * vec4(vec3(inPos.xyz), 1.0);
}

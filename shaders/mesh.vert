#version 450
#include "common.glsl"

// Voxel meshes: integer corner positions in model units, a face-normal index,
// and a per-vertex colour. The model matrix, tint and emissive arrive as push
// constants so each monster is one draw.
layout(location = 0) in ivec4 inPos;     // xyz corner, w normal index (0..5)
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform PC {
    mat4 model;
    vec4 color;
    vec4 emissive;
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vColor;

const vec3 kNormals[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));

void main() {
    vec4 world = pc.model * vec4(vec3(inPos.xyz), 1.0);
    vWorldPos = world.xyz;
    vNormal = normalize(mat3(pc.model) * kNormals[clamp(inPos.w, 0, 5)]);
    vColor = inColor * pc.color;
    gl_Position = u.viewProj * world;
}

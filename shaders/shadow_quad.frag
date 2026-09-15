#version 450
#include "common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vAlpha;
void main() {
    if (texture(atlas, vUV).a * vAlpha < 0.5) discard;
}

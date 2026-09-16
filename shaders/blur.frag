#version 450
// 9-tap Gaussian along one axis (dir = 0 makes it a plain bilinear downsample).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec4 p; } pc;   // xy = uv step per tap
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
const float w[5] = float[5](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);
void main() {
    vec3 c = texture(src, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        vec2 o = pc.p.xy * float(i);
        c += texture(src, vUV + o).rgb * w[i];
        c += texture(src, vUV - o).rgb * w[i];
    }
    outColor = vec4(c, 1.0);
}

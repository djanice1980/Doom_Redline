// Shared declarations for all REDLINE shaders.
#extension GL_GOOGLE_include_directive : enable

#define MAX_LIGHTS 32

layout(set = 0, binding = 0, std140) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 cameraPos;     // xyz camera position, w = time (seconds)
    vec4 sunDir;        // xyz direction TOWARDS the sun (normalised), w = intensity
    vec4 ambient;       // rgb ambient colour, w = fog density
    vec4 fogColor;      // rgb, w unused
    vec4 screen;        // width, height, 1/width, 1/height
    vec4 lightPos[MAX_LIGHTS];    // xyz, w = radius
    vec4 lightColor[MAX_LIGHTS];  // rgb, w = intensity
    ivec4 counts;       // x = light count
    mat4 lightViewProj; // sun shadow map projection
    vec4 shadow;        // x = texel size, y = bias, z = strength (0 = off), w = normal offset
} u;

layout(set = 0, binding = 1) uniform sampler2D atlas;
layout(set = 0, binding = 2) uniform sampler2DShadow shadowMap;

// 1 = lit by the sun, 0 = fully shadowed. 3x3 percentage-closer filter.
float sunShadow(vec3 P, vec3 N) {
    if (u.shadow.z <= 0.0) return 1.0;
    vec4 lp = u.lightViewProj * vec4(P + N * u.shadow.w, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    float z = ndc.z - u.shadow.y;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(shadowMap, vec3(uv + vec2(x, y) * u.shadow.x, z));
    float lit = sum / 9.0;
    return mix(1.0, lit, u.shadow.z);
}

// Lambert + Blinn-Phong with a sun and up to MAX_LIGHTS point lights.
vec3 shade(vec3 P, vec3 N, vec3 albedo, float roughness, float metallic) {
    vec3 V = normalize(u.cameraPos.xyz - P);
    vec3 diffuse = u.ambient.rgb * albedo;
    vec3 spec = vec3(0.0);
    float shininess = mix(96.0, 4.0, roughness);
    vec3 specColor = mix(vec3(0.04), albedo, metallic);

    // Sun, shadowed
    {
        vec3 L = normalize(u.sunDir.xyz);
        float ndl = max(dot(N, L), 0.0);
        float sh = ndl > 0.0 ? sunShadow(P, N) : 1.0;
        diffuse += albedo * ndl * u.sunDir.w * sh;
        vec3 H = normalize(L + V);
        spec += specColor * pow(max(dot(N, H), 0.0), shininess) * ndl * u.sunDir.w * sh;
    }
    // Point lights
    for (int i = 0; i < u.counts.x; ++i) {
        vec3 d = u.lightPos[i].xyz - P;
        float dist = length(d);
        float radius = max(u.lightPos[i].w, 0.001);
        float att = clamp(1.0 - dist / radius, 0.0, 1.0);
        att *= att;
        if (att <= 0.0) continue;
        vec3 L = d / max(dist, 0.0001);
        float ndl = max(dot(N, L), 0.0);
        vec3 c = u.lightColor[i].rgb * u.lightColor[i].w * att;
        diffuse += albedo * ndl * c;
        vec3 H = normalize(L + V);
        spec += specColor * pow(max(dot(N, H), 0.0), shininess) * ndl * c;
    }
    return diffuse * (1.0 - metallic * 0.5) + spec;
}

vec3 applyFog(vec3 color, vec3 P) {
    float dist = length(u.cameraPos.xyz - P);
    float f = 1.0 - exp(-dist * u.ambient.w);
    return mix(color, u.fogColor.rgb, clamp(f, 0.0, 1.0));
}

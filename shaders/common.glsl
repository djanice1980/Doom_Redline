// Shared declarations for all REDLINE shaders.
#extension GL_GOOGLE_include_directive : enable

#define MAX_LIGHTS 16

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
} u;

layout(set = 0, binding = 1) uniform sampler2D atlas;

// Lambert + Blinn-Phong with a sun and up to MAX_LIGHTS point lights.
vec3 shade(vec3 P, vec3 N, vec3 albedo, float roughness, float metallic) {
    vec3 V = normalize(u.cameraPos.xyz - P);
    vec3 diffuse = u.ambient.rgb * albedo;
    vec3 spec = vec3(0.0);
    float shininess = mix(96.0, 4.0, roughness);
    vec3 specColor = mix(vec3(0.04), albedo, metallic);

    // Sun
    {
        vec3 L = normalize(u.sunDir.xyz);
        float ndl = max(dot(N, L), 0.0);
        diffuse += albedo * ndl * u.sunDir.w;
        vec3 H = normalize(L + V);
        spec += specColor * pow(max(dot(N, H), 0.0), shininess) * ndl * u.sunDir.w;
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

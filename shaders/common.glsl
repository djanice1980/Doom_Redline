// Shared declarations for all REDLINE shaders.
#extension GL_GOOGLE_include_directive : enable
#ifdef RT_SHADOWS
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
#endif

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
    ivec4 counts;       // x = light count, y = ray-tracing mode (0 off, 1 sun, 2 all lights, 3 + reflections)
    mat4 lightViewProj; // sun shadow map projection
    vec4 shadow;        // x = texel size, y = bias, z = strength (0 = off), w = normal offset
    vec4 misc;          // xy = atlas size in texels, z = colour grade amount
} u;

layout(set = 0, binding = 1) uniform sampler2D atlas;

layout(set = 0, binding = 2) uniform sampler2DShadow shadowMap;

#ifdef RT_SHADOWS
layout(set = 0, binding = 3) uniform accelerationStructureEXT tlas;
// 1 = nothing between origin and origin + dir * tmax, 0 = occluded. Opaque geometry only.
float rtVisibility(vec3 origin, vec3 dir, float tmax) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.01, dir, tmax);
    while (rayQueryProceedEXT(q)) {}
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
#endif

#ifdef RT_SHADOWS
// Ray-traced ambient occlusion: three short rays over the hemisphere, rotated by a position
// hash so the pattern breaks up (three is what the Radeon 8060S affords). 1 = open, 0.3 = boxed in.
float rtAmbientOcclusion(vec3 P, vec3 N) {
    const float radius = 0.7;
    vec3 T = normalize(abs(N.y) < 0.9 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(N, T);
    float noise = fract(sin(dot(P.xz * 37.1 + P.y * 11.7, vec2(12.9898, 78.233))) * 43758.5453);   // per-position hash (works in every stage)
    float occluded = 0.0;
    for (int i = 0; i < 3; ++i) {
        float a = (float(i) + noise) * 2.399963;          // golden angle spiral
        float r = sqrt((float(i) + 0.5 + noise) / 3.0);   // cosine-weighted: denser near the normal
        float z = sqrt(max(0.0, 1.0 - r * r));
        vec3 dir = T * (r * cos(a)) + B * (r * sin(a)) + N * z;
        occluded += 1.0 - rtVisibility(P + N * 0.03, dir, radius);
    }
    return 1.0 - 0.7 * occluded / 3.0;
}
#endif

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
    float ao = 1.0;
#ifdef RT_SHADOWS
    if (u.counts.y > 2) ao = rtAmbientOcclusion(P, N);   // contact shadows in the full (reflections) mode
#endif
    vec3 diffuse = u.ambient.rgb * albedo * ao;
    vec3 spec = vec3(0.0);
    float shininess = mix(96.0, 4.0, roughness);
    vec3 specColor = mix(vec3(0.04), albedo, metallic);

    // Sun, shadowed (ray-traced when the frame asks for it, else the shadow map)
    {
        vec3 L = normalize(u.sunDir.xyz);
        float ndl = max(dot(N, L), 0.0);
        float sh = 1.0;
        if (ndl > 0.0) {
#ifdef RT_SHADOWS
            if (u.counts.y > 0) sh = rtVisibility(P + N * 0.04, L, 200.0);
            else
#endif
            sh = sunShadow(P, N);
        }
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
#ifdef RT_SHADOWS
        // Torches, lamps, muzzle flashes and glowing blocks cast shadows too. The ray stops
        // short of the light so an emitter sitting inside a block (a red cell) does not
        // shadow itself; only lights that matter to this fragment are traced.
        if (u.counts.y > 1 && ndl > 0.0 && att > 0.015) c *= rtVisibility(P + N * 0.04, L, max(0.05, dist - 0.6));
#endif
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

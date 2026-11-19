#include "common.glsl"
#include "atlas_frag.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vColor;
layout(location = 4) in vec4 vEmissive;
layout(location = 5) flat in vec4 vParams;   // flat: see cube.vert
layout(location = 6) in vec3 vTangent;
layout(location = 7) in vec2 vFaceUV;

// Material maps (see Renderer::setMaterialMaps): slot 0 is flat, the range's slot arrives as a push constant.
layout(set = 0, binding = 4) uniform sampler2D matNormal[8];
layout(set = 0, binding = 5) uniform sampler2D matRough[8];
layout(push_constant) uniform CubePC { int material; } cpc;

layout(location = 0) out vec4 outColor;
// The reflection, kept apart from the colour so it can be blended over frames
// (refl_resolve.frag): rgb = what it adds to the colour, fog and all; a = how far the
// reflection ray went (0 = no reflection here), so the blend can find where the
// reflected thing was last frame.
layout(location = 1) out vec4 outRefl;

#ifdef RT_SHADOWS
// Ray-traced reflections: one ray per fragment of a reflective cube; the hit is shaded
// from the instance data (cubes) or the mesh's own vertices (voxel models), with the
// sun shadowed by a second ray and point lights unshadowed.
struct CubeInst { vec4 posScale, color, emissive, uvRect, params, rot; };
layout(set = 0, binding = 6, std430) readonly buffer CubeInsts { CubeInst cubes[]; };
struct MeshInfo { uvec2 vb; uvec2 ib; vec4 color; vec4 emissive; };
layout(set = 0, binding = 7, std430) readonly buffer MeshInfos { MeshInfo meshInfos[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words { uint w[]; };

const vec3 kMeshNormals[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
// Unit-cube faces as built by Renderer::createGeometry: normal, u axis, v axis.
const vec3 kFaceN[6] = vec3[6](vec3(0, 0, 1), vec3(0, 0, -1), vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0));
const vec3 kFaceU[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0));
const vec3 kFaceV[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1));

vec3 shadeHit(vec3 H, vec3 N, vec3 albedo, vec3 emissive, float camDist) {
    vec3 lit = u.ambient.rgb * albedo;
    {
        vec3 L = normalize(u.sunDir.xyz);
        float ndl = max(dot(N, L), 0.0);
        if (ndl > 0.0) lit += albedo * ndl * u.sunDir.w * rtVisibility(H + N * 0.04, L, 200.0);
    }
    for (int i = 0; i < u.counts.x; ++i) {
        vec3 d = u.lightPos[i].xyz - H;
        float dist = length(d);
        float att = clamp(1.0 - dist / max(u.lightPos[i].w, 0.001), 0.0, 1.0);
        att *= att;
        if (att <= 0.0) continue;
        float ndl = max(dot(N, d / max(dist, 0.0001)), 0.0);
        lit += albedo * ndl * u.lightColor[i].rgb * u.lightColor[i].w * att;
    }
    lit += emissive;
    float f = 1.0 - exp(-camDist * u.ambient.w);
    return mix(lit, u.fogColor.rgb, clamp(f, 0.0, 1.0));
}

// rgb = the light coming back along R, a = how far the ray went (80 when it hit nothing).
vec4 traceReflection(vec3 P, vec3 N, vec3 R) {
    vec3 origin = P + N * 0.03;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.02, R, 80.0);
    while (rayQueryProceedEXT(q)) {}
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) return vec4(u.fogColor.rgb, 80.0);
    float t = rayQueryGetIntersectionTEXT(q, true);
    vec3 H = origin + R * t;
    int custom = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
    vec3 hitN, hitAlbedo, hitEmis;
    if ((custom & 0x800000) == 0) {
        CubeInst c = cubes[custom];
        float cy = cos(c.rot.y), sy = sin(c.rot.y), cx = cos(c.rot.x), sx = sin(c.rot.x);
        mat3 ry = mat3(cy, 0.0, -sy,  0.0, 1.0, 0.0,  sy, 0.0, cy);
        mat3 rx = mat3(1.0, 0.0, 0.0,  0.0, cx, sx,  0.0, -sx, cx);
        mat3 Rm = rx * ry;
        vec3 local = transpose(Rm) * (H - c.posScale.xyz) / max(c.posScale.w, 0.0001);
        vec3 a = abs(local);
        int face;
        if (a.x >= a.y && a.x >= a.z) face = local.x > 0.0 ? 2 : 3;
        else if (a.y >= a.z) face = local.y > 0.0 ? 4 : 5;
        else face = local.z > 0.0 ? 0 : 1;
        vec2 uv = clamp(vec2(dot(local, kFaceU[face]) + 0.5, 0.5 - dot(local, kFaceV[face])), 0.0, 1.0);
        vec4 tex = textureLod(atlas, mix(c.uvRect.xy, c.uvRect.zw, uv), 0.0);
        hitN = Rm * kFaceN[face];
        hitAlbedo = tex.rgb * c.color.rgb;
        hitEmis = c.emissive.rgb * c.emissive.w;
    } else {
        MeshInfo mi = meshInfos[custom & 0x7FFFFF];
        int prim = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
        Words ib = Words(mi.ib);
        uint i0 = ib.w[prim * 3];
        Words vb = Words(mi.vb);
        uint w1 = vb.w[i0 * 3u + 1u];       // z | normal index << 16
        uint rgba = vb.w[i0 * 3u + 2u];
        int nidx = bitfieldExtract(int(w1), 16, 16);
        mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(q, true);
        hitN = normalize(mat3(o2w) * kMeshNormals[clamp(nidx, 0, 5)]);
        vec4 col = unpackUnorm4x8(rgba) * mi.color;
        hitAlbedo = col.rgb;
        hitEmis = mi.emissive.rgb * mi.emissive.w;
    }
    if (dot(hitN, R) > 0.0) hitN = -hitN;
    return vec4(shadeHit(H, hitN, hitAlbedo, hitEmis, length(u.cameraPos.xyz - P) + t), t);
}

// A microfacet normal for view direction Ve (tangent space, z = the surface normal) drawn
// from the GGX distribution of roughness a, as the view sees it (Heitz 2018). U: two
// numbers in [0, 1).
vec3 sampleGgxVisibleNormal(vec3 Ve, float a, vec2 U) {
    vec3 Vh = normalize(vec3(a * Ve.x, a * Ve.y, Ve.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);
    float r = sqrt(U.x), phi = 6.2831853 * U.y;
    float t1 = r * cos(phi), t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(1.0 - t1 * t1 - t2 * t2, 0.0)) * Vh;
    return normalize(vec3(a * Nh.x, a * Nh.y, max(Nh.z, 0.0)));
}
#endif

void main() {
    vec4 tex = sampleAtlas(vUV);
    vec3 albedo = tex.rgb * vColor.rgb;
    vec3 N = normalize(vNormal);
    float rough = vParams.x;
    if (cpc.material > 0) {
        vec3 T = normalize(vTangent - N * dot(N, vTangent));
        vec3 B = -cross(N, T);   // texture v grows downwards on every face
        vec3 nm = texture(matNormal[cpc.material], vFaceUV).xyz * 2.0 - 1.0;
        N = normalize(T * nm.x + B * nm.y + N * max(nm.z, 0.15));
        // The pack's roughness maps sit around 0.93 everywhere; only their variation is
        // useful, so they modulate the instance roughness instead of replacing it.
        rough = clamp(rough + (texture(matRough[cpc.material], vFaceUV).r - 0.93) * 2.0, 0.03, 1.0);
    }
    vec3 lit = shade(vWorldPos, N, albedo, rough, vParams.y);
    vec3 reflAdd = vec3(0.0);
    float reflDist = 0.0;
#ifdef RT_SHADOWS
    // flag bit 2: reflective surface (the arena floor). Glossiness comes from the
    // roughness (map included), the view angle adds the usual Fresnel rise at grazing angles.
    if (u.counts.y > 2 && (int(vParams.w) & 2) != 0) {
        vec3 V = normalize(u.cameraPos.xyz - vWorldPos);
        float gloss = 1.0 - rough;
        gloss *= gloss;
        float fres = 0.04 + 0.96 * pow(1.0 - max(dot(N, V), 0.0), 5.0);
        float w = clamp(gloss * mix(0.5 + 0.5 * fres, 1.0, vParams.y), 0.0, 0.9);
        if (w > 0.01) {
            // A glossy reflection: one ray per pixel per frame, scattered over the gloss
            // lobe of the surface (its roughness, map included) about the bumped normal,
            // at a different spot each frame. One frame alone is noisy; the blend over
            // frames (refl_resolve.frag) adds the rays up into the lobe's soft image. A
            // single mirror ray per pixel instead threw neighbouring pixels' rays all over
            // by the bumps, and small bright things (a torch's head, lit point-blank by
            // its own flame) came out as scattered sparks.
            vec3 T = normalize(abs(N.y) < 0.9 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0)));
            vec3 B = cross(N, T);
            vec3 Ve = vec3(dot(V, T), dot(V, B), max(dot(V, N), 1e-3));
            float h = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
            float h2 = fract(sin(dot(gl_FragCoord.xy, vec2(39.3467, 11.135))) * 24634.6345);
            vec2 U = fract(vec2(h, h2) + u.misc.w * vec2(0.7548777, 0.5698403));
            // The lobe is narrower than the roughness alone would make it (GGX alpha = rough^2):
            // the full lobe smeared the floor's torch reflections into faint smudges, while
            // a sixth of it still kills the sparks (it is the blend over frames that does
            // that) and keeps the reflections readable.
            float a = max(rough * rough * 0.15, 0.002);
            vec3 Hm = sampleGgxVisibleNormal(Ve, a, U);
            vec3 Hw = normalize(T * Hm.x + B * Hm.y + N * Hm.z);
            vec3 R = reflect(-V, Hw);
            // Under the surface (a grazing view through a steep microfacet): fold it back up.
            vec3 Ng = normalize(vNormal);
            if (dot(R, Ng) < 0.02) R = normalize(R - Ng * (2.0 * dot(R, Ng) - 0.02));
            vec4 refl = traceReflection(vWorldPos, N, R);
            lit *= 1.0 - w;
            reflAdd = refl.rgb * mix(vec3(1.0), albedo, vParams.y) * w;
            reflDist = refl.a;
        }
    }
#endif
    // flag bit 1: pulse emissive with time as well
    float e = vEmissive.w;
    if ((int(vParams.w) & 1) != 0) e *= 0.75 + 0.25 * sin(u.cameraPos.w * 6.0 + vParams.z);
    lit += vEmissive.rgb * e;
    lit = applyFog(lit, vWorldPos);
    outColor = vec4(lit, vColor.a * tex.a);
    // The fog takes the same share of the reflection as of the rest (applyFog is a mix).
    float fogF = clamp(1.0 - exp(-length(u.cameraPos.xyz - vWorldPos) * u.ambient.w), 0.0, 1.0);
    outRefl = vec4(reflAdd * (1.0 - fogF), reflDist);
}

// Fragment-only atlas sampling (needs derivatives); include after common.glsl.
// Pixel-art sampling: bilinear filtering whose transition between texels is squeezed to one
// screen pixel, so magnified textures stay crisp and minified ones use the mips without shimmer.
vec4 sampleAtlas(vec2 uv) {
    vec2 size = u.misc.xy;
    vec2 texel = uv * size;
    vec2 w = clamp(fwidth(texel), 1e-4, 1.0);
    vec2 sharp = floor(texel) + 0.5 + clamp((fract(texel) - 0.5) / w, -0.5, 0.5);
    return textureGrad(atlas, sharp / size, dFdx(uv), dFdy(uv));
}

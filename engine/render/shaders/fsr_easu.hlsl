// FidelityFX Super Resolution 1's first pass, edge-adaptive spatial upsampling (EASU): each pixel
// of the output from the 12 texels of the render around it, with a Lanczos-like kernel stretched
// along the edges it finds. Ported from the 32-bit path of AMD's ffx_fsr1.h (FsrEasuF), with the
// gathers as texel reads; the "fsr" upscaler (src/upscaler.cpp) draws it as a canvas shader over
// a target of the output size, with TEXTURE the render, filtered nearest.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// ffx_a.h's APrxLoRcpF1 and APrxLoRsqF1: a reciprocal and a reciprocal square root from the
// bits, finite at 0.
float easu_rcp(float a) {
    return asfloat(0x7ef07ebbu - asuint(a));
}

float easu_rsq(float a) {
    return asfloat(0x5f347d74u - (asuint(a) >> 1u));
}

// The texel at a position of the render, clamped to its edges by the sampler.
float4 easu_texel(float2 at) {
    return texture(TEXTURE, (at + 0.5) * TEXTURE_PIXEL_SIZE);
}

// Luma times 2, the cheapest FSR takes.
float easu_luma(float3 c) {
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

// FsrEasuSetF: adds the direction and length of the edge at c, from the cross around it, with
// bilinear weight w.
//    a
//  b c d
//    e
void easu_set(inout float2 dir, inout float len, float w, float la, float lb, float lc, float ld,
              float le) {
    float dc = ld - lc;
    float cb = lc - lb;
    float len_x = max(abs(dc), abs(cb));
    len_x = easu_rcp(len_x);
    float dir_x = ld - lb;
    dir.x += dir_x * w;
    len_x = saturate(abs(dir_x) * len_x);
    len_x *= len_x;
    len += len_x * w;
    float ec = le - lc;
    float ca = lc - la;
    float len_y = max(abs(ec), abs(ca));
    len_y = easu_rcp(len_y);
    float dir_y = le - la;
    dir.y += dir_y * w;
    len_y = saturate(abs(dir_y) * len_y);
    len_y *= len_y;
    len += len_y * w;
}

// FsrEasuTapF: adds one texel, weighted by its offset turned along the edge.
void easu_tap(inout float3 color, inout float weight, float2 off, float2 dir, float2 len,
              float lob, float clp, float3 c) {
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    // (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lob * x^2 - 1)^2, Lanczos 2 without a sine.
    float wb = (2.0 / 5.0) * d2 + (-1.0);
    float wa = lob * d2 + (-1.0);
    wb *= wb;
    wa *= wa;
    wb = (25.0 / 16.0) * wb + (-(25.0 / 16.0 - 1.0));
    float w = wb * wa;
    color += c * w;
    weight += w;
}

void fragment() {
    // Where the pixel's center falls in the render, from its top left texel f.
    float2 size = round(1.0 / TEXTURE_PIXEL_SIZE);
    float2 pp = UV * size - 0.5;
    float2 fp = floor(pp);
    pp -= fp;
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    float3 b = easu_texel(fp + float2(0.0, -1.0)).rgb;
    float3 c = easu_texel(fp + float2(1.0, -1.0)).rgb;
    float3 e = easu_texel(fp + float2(-1.0, 0.0)).rgb;
    float4 f4 = easu_texel(fp);
    float3 f = f4.rgb;
    float3 g = easu_texel(fp + float2(1.0, 0.0)).rgb;
    float3 h = easu_texel(fp + float2(2.0, 0.0)).rgb;
    float3 i = easu_texel(fp + float2(-1.0, 1.0)).rgb;
    float3 j = easu_texel(fp + float2(0.0, 1.0)).rgb;
    float3 k = easu_texel(fp + float2(1.0, 1.0)).rgb;
    float3 l = easu_texel(fp + float2(2.0, 1.0)).rgb;
    float3 n = easu_texel(fp + float2(0.0, 2.0)).rgb;
    float3 o = easu_texel(fp + float2(1.0, 2.0)).rgb;
    float bl = easu_luma(b);
    float cl = easu_luma(c);
    float el = easu_luma(e);
    float fl = easu_luma(f);
    float gl = easu_luma(g);
    float hl = easu_luma(h);
    float il = easu_luma(i);
    float jl = easu_luma(j);
    float kl = easu_luma(k);
    float ll = easu_luma(l);
    float nl = easu_luma(n);
    float ol = easu_luma(o);

    // The edge's direction and length, bilinearly from the four texels around the pixel.
    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bl, el, fl, gl, jl);
    easu_set(dir, len, pp.x * (1.0 - pp.y), cl, fl, gl, hl, kl);
    easu_set(dir, len, (1.0 - pp.x) * pp.y, fl, il, jl, kl, nl);
    easu_set(dir, len, pp.x * pp.y, gl, jl, kl, ll, ol);
    float2 dir2 = dir * dir;
    float dir_r = dir2.x + dir2.y;
    bool zero = dir_r < (1.0 / 32768.0);
    dir_r = easu_rsq(dir_r);
    dir_r = zero ? 1.0 : dir_r;
    dir.x = zero ? 1.0 : dir.x;
    dir *= dir_r;
    len = len * 0.5;
    len *= len;
    // The kernel stretches from 1 along the axes to sqrt(2) on diagonals, and narrows across
    // edges, its window shifting from sqrt(2) to just past 2.
    float stretch = (dir.x * dir.x + dir.y * dir.y) * easu_rcp(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = easu_rcp(lob);

    float3 min4 = min(min(f, g), min(j, k));
    float3 max4 = max(max(f, g), max(j, k));
    float3 color = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    easu_tap(color, weight, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easu_tap(color, weight, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easu_tap(color, weight, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easu_tap(color, weight, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easu_tap(color, weight, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easu_tap(color, weight, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easu_tap(color, weight, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easu_tap(color, weight, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easu_tap(color, weight, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easu_tap(color, weight, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easu_tap(color, weight, float2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easu_tap(color, weight, float2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    // Normalized, and kept within the four nearest texels so edges don't ring.
    COLOR = float4(min(max4, max(min4, color * (1.0 / weight))), f4.a);
}

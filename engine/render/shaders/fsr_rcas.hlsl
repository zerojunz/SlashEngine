// FidelityFX Super Resolution 1's second pass, robust contrast-adaptive sharpening (RCAS): each
// pixel sharpened against the four around it, as far as they allow without clipping. Ported from
// the 32-bit path of AMD's ffx_fsr1.h (FsrRcasF, alpha passed through, no denoising); the "fsr"
// upscaler (src/upscaler.cpp) draws it as a canvas shader over the screen, with TEXTURE the
// EASU pass's output at the screen's size, filtered nearest, and the param sharpness
// 2^-fsr_sharpness (FsrRcasCon): 1 is the sharpest.
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

// FSR_RCAS_LIMIT: how far the lobe may go.
static const float RCAS_LIMIT = 0.25 - (1.0 / 16.0);

// ffx_a.h's APrxMedRcpF1: a reciprocal from the bits, refined once.
float rcas_rcp(float a) {
    float b = asfloat(0x7ef19fffu - asuint(a));
    return b * (-b * a + 2.0);
}

// The pixel at an offset of whole pixels from this one.
float3 rcas_texel(float2 offset) {
    return texture(TEXTURE, UV + offset * TEXTURE_PIXEL_SIZE).rgb;
}

// One channel's lobe: as far as the ring's minimum and maximum let it sharpen. A ring of all 0
// or all 1 divides 0 by 0 in AMD's code, and GPUs' max leaves the NaN out, so so does this.
float rcas_lobe(float mn4, float mx4, float e) {
    float hit_max_den = 4.0 * mn4 - 4.0;
    if (mx4 <= 0.0) {
        return (1.0 - max(mx4, e)) / hit_max_den;
    }
    float hit_min = min(mn4, e) / (4.0 * mx4);
    if (hit_max_den >= 0.0) {
        return -hit_min;
    }
    float hit_max = (1.0 - max(mx4, e)) / hit_max_den;
    return max(-hit_min, hit_max);
}

void fragment() {
    //    b
    //  d e f
    //    h
    float3 b = rcas_texel(float2(0.0, -1.0));
    float3 d = rcas_texel(float2(-1.0, 0.0));
    float4 ee = texture(TEXTURE, UV);
    float3 e = ee.rgb;
    float3 f = rcas_texel(float2(1.0, 0.0));
    float3 h = rcas_texel(float2(0.0, 1.0));
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float lobe_r = rcas_lobe(mn4.r, mx4.r, e.r);
    float lobe_g = rcas_lobe(mn4.g, mx4.g, e.g);
    float lobe_b = rcas_lobe(mn4.b, mx4.b, e.b);
    float lobe = max(-RCAS_LIMIT, min(max(max(lobe_r, lobe_g), lobe_b), 0.0)) * sharpness;
    float rcp_l = rcas_rcp(4.0 * lobe + 1.0);
    COLOR = float4((lobe * b + lobe * d + lobe * h + lobe * f + e) * rcp_l, ee.a);
}

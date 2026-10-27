// FSR 2's sharpening pass (ffx_fsr2_rcas.h): the accumulate pass's color sharpened with FSR 1's
// robust contrast-adaptive sharpening, denoised, as FSR 2's copy of ffx_fsr1.h has it; or copied
// as it is where Godot turns sharpening off (a sharpness of 0), as the accumulate pass writes the
// output itself then. Keep it in step with src/fsr2.cpp, its CPU twin.
//
// Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.
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

#include "fsr2.hlsl"

// The accumulate pass's color at the display size.
Texture2D<float4> internal_color : register(t0, space2);
SamplerState internal_color_sampler : register(s0, space2);

static const float FSR_RCAS_LIMIT = 0.25 - (1.0 / 16.0);

float3 FsrRcasLoadF(int2 p) {
    float3 fColor = internal_color.Load(int3(Inside(p, DisplaySize()), 0)).rgb;
    return PrepareRgb(fColor, Exposure(), PreExposure());
}

// ffxApproximateReciprocalMedium: a reciprocal from the bits, refined once.
float RcpMedium(float a) {
    float b = asfloat(0x7ef19fffu - asuint(a));
    return b * (-b * a + 2.0);
}

// One channel's lobe, max(-hitMin, hitMax). Where AMD's reciprocals divide by 0, a NaN or an
// infinity that GPUs' max leaves out, the other side is taken.
float RcasLobe(float mn4, float mx4) {
    float fHitMaxDen = 4.0 * mn4 - 4.0;
    if (mx4 <= 0.0) {
        return (1.0 - mx4) / fHitMaxDen;
    }
    float fHitMin = mn4 / (4.0 * mx4);
    if (fHitMaxDen == 0.0) {
        return -fHitMin;
    }
    return max(-fHitMin, (1.0 - mx4) / fHitMaxDen);
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 sp = int2(position.xy);
    if (options.z == 1) {
        return float4(internal_color.Load(int3(sp, 0)).rgb, 1.0);
    }
    //    b
    //  d e f
    //    h
    float3 b = FsrRcasLoadF(sp + int2(0, -1));
    float3 d = FsrRcasLoadF(sp + int2(-1, 0));
    float3 e = FsrRcasLoadF(sp);
    float3 f = FsrRcasLoadF(sp + int2(1, 0));
    float3 h = FsrRcasLoadF(sp + int2(0, 1));
    // Luma times 2.
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);
    // Noise detection.
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    float fRange = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
    nz = ffxSaturate(abs(nz) * RcpMedium(fRange));
    nz = -0.5 * nz + 1.0;
    // Min and max of the ring.
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float lobeR = RcasLobe(mn4.r, mx4.r);
    float lobeG = RcasLobe(mn4.g, mx4.g);
    float lobeB = RcasLobe(mn4.b, mx4.b);
    float lobe = max(-FSR_RCAS_LIMIT, min(max(max(lobeR, lobeG), lobeB), 0.0)) * frame.w;
    lobe *= nz;
    // The resolve, with the medium precision reciprocal that keeps tones from changing.
    float rcpL = RcpMedium(4.0 * lobe + 1.0);
    float3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    return float4(UnprepareRgb(pix, Exposure()), 1.0);
}

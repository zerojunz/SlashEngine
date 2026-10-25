// FSR 2's luminance pass (ffx_fsr2_compute_luminance_pyramid.h), drawing the level of the log luma
// pyramid that the accumulate pass reads shading changes from. AMD's single pass downsampler makes
// every level, for an auto exposure Godot doesn't turn on; this pass draws only that level, 1/32
// of the render size, each texel the average of the 32x32 log lumas under it (none outside the
// render size), as the downsampler's averages of averages come to. Keep it in step with
// src/fsr2.cpp, its CPU twin.
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

// The frame's 3D world at the render size, drawn with the jitter.
Texture2D<float4> input_color : register(t0, space2);
SamplerState input_color_sampler : register(s0, space2);

float3 LoadInputColor(int2 iPxPos) {
    return input_color.Load(int3(Inside(iPxPos, RenderSize()), 0)).rgb;
}

// SampleInputColor: the world filtered linearly at uv, clamped to its edges.
float3 SampleInputColor(float2 fUv) {
    int2 iBase;
    float2 fFrac = LinearWeights(fUv, RenderSize(), iBase);
    float3 fTop = ffxLerp(LoadInputColor(iBase), LoadInputColor(iBase + int2(1, 0)), fFrac.xxx);
    float3 fBottom =
        ffxLerp(LoadInputColor(iBase + int2(0, 1)), LoadInputColor(iBase + int2(1, 1)), fFrac.xxx);
    return ffxLerp(fTop, fBottom, fFrac.yyy);
}

// SpdLoadSourceImage: a render pixel's log luma, 0 outside the render size.
float SourceLogLuma(int2 iPx) {
    if (any(iPx >= RenderSize())) {
        return 0.0;
    }
    float2 fUv = (float2(iPx) + 0.5 + Jitter()) / float2(RenderSize());
    fUv = ClampUv(fUv, RenderSize(), RenderSize());
    float3 fRgb = SampleInputColor(fUv);
    fRgb /= PreExposure();
    return log(max(FSR2_EPSILON, RGBToLuma(fRgb)));
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 iMipPos = int2(position.xy);
    float fSum = 0.0;
    [loop] for (int y = 0; y < 32; ++y) {
        float fRow = 0.0;
        [loop] for (int x = 0; x < 32; ++x) {
            fRow += SourceLogLuma(iMipPos * 32 + int2(x, y));
        }
        fSum += fRow;
    }
    return float4(fSum / 1024.0, 0.0, 0.0, 0.0);
}

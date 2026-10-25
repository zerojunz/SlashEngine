// FSR 3's shading change pass (ffx_fsr3upscaler_shading_change.h), at half the render size: how
// much the shading changed around each texel since the frame before, the most of the shading
// change pyramid's first three levels, each the luma difference times its sign's average. Keep it
// in step with src/fsr3.cpp, its CPU twin.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
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

#include "fsr3.hlsl"

// The shading change pyramid's first three levels.
Texture2D<float4> level0 : register(t0, space2);
Texture2D<float4> level1 : register(t1, space2);
Texture2D<float4> level2 : register(t2, space2);
SamplerState level0_sampler : register(s0, space2);
SamplerState level1_sampler : register(s1, space2);
SamplerState level2_sampler : register(s2, space2);

#define FETCH_LEVEL0(p) level0.Load(int3(p, 0))
#define FETCH_LEVEL1(p) level1.Load(int3(p, 0))
#define FETCH_LEVEL2(p) level2.Load(int3(p, 0))
FSR3_LINEAR_SAMPLE(SampleLevel0, FETCH_LEVEL0)
FSR3_LINEAR_SAMPLE(SampleLevel1, FETCH_LEVEL1)
FSR3_LINEAR_SAMPLE(SampleLevel2, FETCH_LEVEL2)

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 iPxPos = int2(position.xy);
    float2 fUv = (float2(iPxPos) + 0.5) / float2(ShadingChangeRenderSize());
    float2 fUvJittered = fUv + Jitter() / float2(RenderSize());
    // ComputeShadingChangeLuma, with the levels sampled at the first's coordinates.
    float2 fMipUv = ClampUv(fUvJittered, ShadingChangeRenderSize(), GetSPDMipDimensions(0));
    float fSamples[3];
    float2 fSample = SampleLevel0(fMipUv, GetSPDMipDimensions(0)).xy;
    fSamples[0] = abs(fSample.x * fSample.y);
    fSample = SampleLevel1(fMipUv, GetSPDMipDimensions(1)).xy;
    fSamples[1] = abs(fSample.x * fSample.y);
    fSample = SampleLevel2(fMipUv, GetSPDMipDimensions(2)).xy;
    fSamples[2] = abs(fSample.x * fSample.y);
    float fShadingChange = 0.0;
    for (int iMipLevel = 0; iMipLevel < 3; iMipLevel++) {
        if (fSamples[iMipLevel] > 0.0) {
            fShadingChange = max(fShadingChange, fSamples[iMipLevel]);
        }
    }
    return float4(ffxSaturate(fShadingChange), 0.0, 0.0, 0.0);
}

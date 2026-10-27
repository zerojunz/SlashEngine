// FSR 2's reconstruct pass, the scattering half of
// ffx_fsr2_reconstruct_dilated_velocity_and_previous_depth.h: each render pixel's dilated depth
// moved by its motion vector to where it was in the frame before, kept where it is nearest in a
// buffer of depths as bits, which the depth clip pass reads. A compute shader, for its atomics, run
// twice a frame: first to clear the buffer (AMD's lock pass clears it after use), then to scatter.
// Keep it in step with src/fsr2.cpp, its CPU twin.
//
// Metal has one list of buffers, uniforms first, so its SPIR-V is compiled with SLASH_MSL and
// numbers the buffer after the uniforms. The others keep SDL's sets: sampled textures in 0,
// read-write buffers in 1, uniforms in 2.
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

#ifdef SLASH_MSL
#define FSR2_UNIFORMS register(b0)
#define RECONSTRUCTED register(u1)
#else
#define FSR2_UNIFORMS register(b0, space2)
#define RECONSTRUCTED register(u0, space1)
#endif

#include "fsr2.hlsl"

// The dilate pass's depths and motion vectors.
Texture2D<float4> dilated_depth : register(t0, space0);
Texture2D<float4> dilated_motion : register(t1, space0);
SamplerState dilated_depth_sampler : register(s0, space0);
SamplerState dilated_motion_sampler : register(s1, space0);

// The depths as bits, a row of the render size after another: reversed depths are 0 or more, so
// the larger bits are the nearer depth.
RWStructuredBuffer<uint> reconstructed : RECONSTRUCTED;

void StoreReconstructedDepth(int2 iPxSample, float fDepth) {
    uint uOriginal;
    InterlockedMax(reconstructed[iPxSample.y * RenderSize().x + iPxSample.x], asuint(fDepth),
                   uOriginal);
}

void ReconstructPrevDepth(int2 iPxPos, float fDepth, float2 fMotionVector, int2 iPxDepthSize) {
    fMotionVector *= float(length(fMotionVector * float2(DisplaySize())) > 0.1);
    float2 fUv = (float2(iPxPos) + 0.5) / float2(iPxDepthSize);
    float2 fReprojectedUv = fUv + fMotionVector;
    BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fReprojectedUv, RenderSize());
    for (int iSampleIndex = 0; iSampleIndex < 4; iSampleIndex++) {
        int2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
        float fWeight = bilinearInfo.fWeights[iSampleIndex];
        if (fWeight > fReconstructedDepthBilinearWeightThreshold) {
            int2 iStorePos = bilinearInfo.iBasePos + iOffset;
            if (IsOnScreen(iStorePos, iPxDepthSize)) {
                StoreReconstructedDepth(iStorePos, fDepth);
            }
        }
    }
}

[numthreads(8, 8, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {
    int2 iPxLrPos = int2(id.xy);
    if (any(iPxLrPos >= RenderSize())) {
        return;
    }
    if (options.z == 0) {
        reconstructed[iPxLrPos.y * RenderSize().x + iPxLrPos.x] = 0u;
        return;
    }
    float fDilatedDepth = dilated_depth.Load(int3(iPxLrPos, 0)).r;
    float2 fDilatedMotionVector = dilated_motion.Load(int3(iPxLrPos, 0)).xy;
    ReconstructPrevDepth(iPxLrPos, fDilatedDepth, fDilatedMotionVector, RenderSize());
}

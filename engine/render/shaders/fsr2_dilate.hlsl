// FSR 2's dilate pass, the drawing half of ffx_fsr2_reconstruct_dilated_velocity_and_previous_depth.h:
// each render pixel's nearest depth of the 3x3 around it, the motion vector where that depth is,
// and the luma that locks thin features. The other half, which scatters the depths to where they
// were in the frame before, is fsr2_reconstruct.hlsl, a compute shader for its atomics. Keep it in
// step with src/fsr2.cpp, its CPU twin.
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

// The prepass's depth (reversed: 1 at the near plane) and motion vectors, and the world.
Texture2D<float4> input_depth : register(t0, space2);
Texture2D<float4> input_motion : register(t1, space2);
Texture2D<float4> input_color : register(t2, space2);
SamplerState input_depth_sampler : register(s0, space2);
SamplerState input_motion_sampler : register(s1, space2);
SamplerState input_color_sampler : register(s2, space2);

float LoadInputDepth(int2 iPxPos) {
    return input_depth.Load(int3(Inside(iPxPos, RenderSize()), 0)).r;
}
// The motion vectors are already in texture coordinates: AMD's scale is 1.
float2 LoadInputMotionVector(int2 iPxPos) {
    return input_motion.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
float3 LoadInputColor(int2 iPxPos) {
    return input_color.Load(int3(Inside(iPxPos, RenderSize()), 0)).rgb;
}

void FindNearestDepth(int2 iPxPos, int2 iPxSize, out float fNearestDepth,
                      out int2 fNearestDepthCoord) {
    const int2 iSampleOffsets[9] = {int2(+0, +0), int2(+1, +0), int2(+0, +1),
                                    int2(+0, -1), int2(-1, +0), int2(-1, +1),
                                    int2(+1, +1), int2(-1, -1), int2(+1, -1)};
    fNearestDepthCoord = iPxPos;
    fNearestDepth = LoadInputDepth(iPxPos);
    for (int iSampleIndex = 1; iSampleIndex < 9; ++iSampleIndex) {
        int2 iPos = iPxPos + iSampleOffsets[iSampleIndex];
        if (IsOnScreen(iPos, iPxSize)) {
            float fNdDepth = LoadInputDepth(iPos);
            if (fNdDepth > fNearestDepth) {
                fNearestDepthCoord = iPos;
                fNearestDepth = fNdDepth;
            }
        }
    }
}

float ComputeLockInputLuma(int2 iPxLrPos) {
    float3 fRgb = max(float3(0.0, 0.0, 0.0), LoadInputColor(iPxLrPos));
    fRgb /= PreExposure();
    fRgb *= Exposure();
    fRgb = Tonemap(fRgb);
    return ffxPow(RGBToPerceivedLuma(fRgb), 1.0 / 6.0);
}

struct Dilated {
    float4 depth : SV_Target0;
    float4 motion : SV_Target1;
    float4 lock_luma : SV_Target2;
};

Dilated fs_main(float4 position : SV_Position) {
    int2 iPxLrPos = int2(position.xy);
    float fDilatedDepth;
    int2 iNearestDepthCoord;
    FindNearestDepth(iPxLrPos, RenderSize(), fDilatedDepth, iNearestDepthCoord);
    Dilated result;
    result.depth = float4(fDilatedDepth, 0.0, 0.0, 0.0);
    result.motion = float4(LoadInputMotionVector(iNearestDepthCoord), 0.0, 0.0);
    result.lock_luma = float4(ComputeLockInputLuma(iPxLrPos), 0.0, 0.0, 0.0);
    return result;
}

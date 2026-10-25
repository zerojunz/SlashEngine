// FSR 3's accumulate pass (ffx_fsr3upscaler_accumulate.h, with ffx_fsr3upscaler_reproject.h and
// ffx_fsr3upscaler_upsample.h): each display pixel's color from the frame before, moved by its
// motion vector and clamped to the colors around it this frame unless it is locked or flickers,
// blended with this frame's render pixels around it by a Lanczos filter, by how many frames it has
// gathered. Its color and lock are the next frame's history; FSR 2's sharpening pass, which FSR 3's
// matches, sharpens it into the frame. Keep it in step with src/fsr3.cpp, its CPU twin.
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

// The world, the prepare inputs pass's motion vectors, the luma instability, the farthest depths
// at half size, the prepare reactivity pass's masks, the frame before's color and locks, and the
// new locks.
Texture2D<float4> input_color : register(t0, space2);
Texture2D<float4> dilated_motion : register(t1, space2);
Texture2D<float4> luma_instability : register(t2, space2);
Texture2D<float4> farthest_depth : register(t3, space2);
Texture2D<float4> reactive_masks : register(t4, space2);
Texture2D<float4> history : register(t5, space2);
Texture2D<float4> new_locks : register(t6, space2);
SamplerState input_color_sampler : register(s0, space2);
SamplerState dilated_motion_sampler : register(s1, space2);
SamplerState luma_instability_sampler : register(s2, space2);
SamplerState farthest_depth_sampler : register(s3, space2);
SamplerState reactive_masks_sampler : register(s4, space2);
SamplerState history_sampler : register(s5, space2);
SamplerState new_locks_sampler : register(s6, space2);

float3 LoadInputColor(int2 iPxPos) {
    return input_color.Load(int3(Inside(iPxPos, RenderSize()), 0)).rgb;
}
float2 LoadDilatedMotionVector(int2 iPxPos) {
    return dilated_motion.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
float LoadRwNewLocks(int2 iPxPos) {
    return new_locks.Load(int3(Inside(iPxPos, UpscaleSize()), 0)).r;
}
#define FETCH_INSTABILITY(p) luma_instability.Load(int3(p, 0))
#define FETCH_FARTHEST(p) farthest_depth.Load(int3(p, 0))
#define FETCH_MASKS(p) reactive_masks.Load(int3(p, 0))
#define FETCH_HISTORY(p) history.Load(int3(p, 0))
FSR3_LINEAR_SAMPLE(SampleLumaInstability, FETCH_INSTABILITY)
FSR3_LINEAR_SAMPLE(SampleFarthestDepthMip1, FETCH_FARTHEST)
FSR3_LINEAR_SAMPLE(SampleDilatedReactiveMasks, FETCH_MASKS)
FSR3_LANCZOS2_SAMPLE(HistorySample, FETCH_HISTORY)

struct AccumulationPassCommonParams {
    int2 iPxHrPos;
    float2 fHrUv;
    float2 fLrUvJittered;
    float2 fLrUv_HwSampler;
    float2 fMotionVector;
    float2 fReprojectedHrUv;
    float f4KVelocity;
    float fDisocclusion;
    float fReactiveMask;
    float fShadingChange;
    float fAccumulation;
    float fLumaInstabilityFactor;
    float fFarthestDepthInMeters;
    bool bIsExistingSample;
    bool bIsNewSample;
};

struct RectificationBox {
    float3 boxCenter;
    float3 boxVec;
    float3 aabbMin;
    float3 aabbMax;
    float fBoxCenterWeight;
};

struct AccumulationPassData {
    RectificationBox clippingBox;
    float3 fUpsampledColor;
    float fUpsampledWeight;
    float3 fHistoryColor;
    float fHistoryWeight;
    float fLock;
    float fLockContributionThisFrame;
};

void RectificationBoxAddSample(bool bInitialSample, inout RectificationBox rectificationBox,
                               float3 colorSample, float fSampleWeight) {
    if (bInitialSample) {
        rectificationBox.aabbMin = colorSample;
        rectificationBox.aabbMax = colorSample;
        float3 weightedSample = colorSample * fSampleWeight;
        rectificationBox.boxCenter = weightedSample;
        rectificationBox.boxVec = colorSample * weightedSample;
        rectificationBox.fBoxCenterWeight = fSampleWeight;
    } else {
        rectificationBox.aabbMin = min(rectificationBox.aabbMin, colorSample);
        rectificationBox.aabbMax = max(rectificationBox.aabbMax, colorSample);
        float3 weightedSample = colorSample * fSampleWeight;
        rectificationBox.boxCenter += weightedSample;
        rectificationBox.boxVec += colorSample * weightedSample;
        rectificationBox.fBoxCenterWeight += fSampleWeight;
    }
}

void RectificationBoxComputeVarianceBoxData(inout RectificationBox rectificationBox) {
    rectificationBox.fBoxCenterWeight =
        (abs(rectificationBox.fBoxCenterWeight) > FSR3UPSCALER_FP32_MIN
             ? rectificationBox.fBoxCenterWeight
             : 1.0);
    rectificationBox.boxCenter /= rectificationBox.fBoxCenterWeight;
    rectificationBox.boxVec /= rectificationBox.fBoxCenterWeight;
    float3 stdDev = sqrt(abs(rectificationBox.boxVec -
                             rectificationBox.boxCenter * rectificationBox.boxCenter));
    rectificationBox.boxVec = stdDev;
}

void InitPassData(int2 iPxHrPos, out AccumulationPassCommonParams params,
                  out AccumulationPassData data) {
    params.iPxHrPos = iPxHrPos;
    float2 fHrUv = (float2(iPxHrPos) + 0.5) / float2(UpscaleSize());
    params.fHrUv = fHrUv;
    params.fLrUvJittered = fHrUv + Jitter() / float2(RenderSize());
    params.fLrUv_HwSampler = ClampUv(params.fLrUvJittered, RenderSize(), MaxRenderSize());
    // GetMotionVector with motion vectors at the render size.
    params.fMotionVector = LoadDilatedMotionVector(int2(fHrUv * float2(RenderSize())));
    params.f4KVelocity = Get4KVelocity(params.fMotionVector);
    // ComputeReprojectedUVs
    params.fReprojectedHrUv = params.fHrUv + params.fMotionVector;
    params.bIsExistingSample = IsUvInside(params.fReprojectedHrUv);
    float2 fLumaInstabilityUv_HW = ClampUv(fHrUv, RenderSize(), MaxRenderSize());
    params.fLumaInstabilityFactor = SampleLumaInstability(fLumaInstabilityUv_HW, RenderSize()).r;
    float2 fFarthestDepthUv =
        ClampUv(params.fLrUvJittered, ShadingChangeRenderSize(), ShadingChangeRenderSize());
    params.fFarthestDepthInMeters =
        SampleFarthestDepthMip1(fFarthestDepthUv, ShadingChangeRenderSize()).r;
    params.bIsNewSample = (params.bIsExistingSample == false || FrameIndex() == 0.0);
    float4 fDilatedReactiveMasks = SampleDilatedReactiveMasks(params.fLrUv_HwSampler, RenderSize());
    params.fReactiveMask = ffxSaturate(fDilatedReactiveMasks[REACTIVE]);
    params.fDisocclusion = ffxSaturate(fDilatedReactiveMasks[DISOCCLUSION]);
    params.fShadingChange = ffxSaturate(fDilatedReactiveMasks[SHADING_CHANGE]);
    params.fAccumulation = ffxSaturate(fDilatedReactiveMasks[ACCUMULAION]);
    params.fAccumulation *= float(RoundsAboveOne(params.fAccumulation * 100.0));
    data.clippingBox.boxCenter = float3(0.0, 0.0, 0.0);
    data.clippingBox.boxVec = float3(0.0, 0.0, 0.0);
    data.clippingBox.aabbMin = float3(0.0, 0.0, 0.0);
    data.clippingBox.aabbMax = float3(0.0, 0.0, 0.0);
    data.clippingBox.fBoxCenterWeight = 0.0;
    data.fUpsampledColor = float3(0.0, 0.0, 0.0);
    data.fHistoryColor = float3(0.0, 0.0, 0.0);
    data.fHistoryWeight = 1.0;
    data.fUpsampledWeight = 0.0;
    data.fLock = 0.0;
    data.fLockContributionThisFrame = 0.0;
}

void ReprojectHistoryColor(AccumulationPassCommonParams params, inout AccumulationPassData data) {
    float4 fReprojectedHistory = HistorySample(params.fReprojectedHrUv, PreviousFrameUpscaleSize());
    data.fHistoryColor = fReprojectedHistory.rgb;
    data.fHistoryColor *= DeltaPreExposure();
    data.fHistoryColor *= Exposure();
    data.fHistoryColor = RGBToYCoCg(data.fHistoryColor);
    data.fLock = fReprojectedHistory.w;
}

void UpdateLockStatus(AccumulationPassCommonParams params, inout AccumulationPassData data) {
    data.fLock *= float(params.bIsNewSample == false);
    float fLifetimeDecreaseFactor = max(ffxSaturate(params.fShadingChange),
                                        max(params.fReactiveMask, params.fDisocclusion));
    data.fLock = max(0.0, data.fLock - fLifetimeDecreaseFactor * fLockMax);
    // This frame's lock contribution.
    data.fLockContributionThisFrame =
        ffxSaturate(ffxSaturate(data.fLock - fLockThreshold) * (fLockMax - fLockThreshold));
    float fNewLockIntensity = LoadRwNewLocks(params.iPxHrPos) *
                              (1.0 - max(params.fShadingChange * 0.0, params.fReactiveMask));
    data.fLock = max(0.0, min(data.fLock + fNewLockIntensity, fLockMax));
    // Ready for the next frame.
    float fLifetimeDecrease = (0.1 / JitterSequenceLength()) * (1.0 - fLifetimeDecreaseFactor);
    data.fLock = max(0.0, data.fLock - fLifetimeDecrease);
    // A lock that will leave the screen next frame, by this one's motion, goes, so that locks
    // aren't clamped to the edges.
    float2 fEstimatedUvNextFrame = params.fHrUv - params.fMotionVector;
    data.fLock *= float(IsUvInside(fEstimatedUvNextFrame) == true);
}

void ComputeBaseAccumulationWeight(AccumulationPassCommonParams params,
                                   inout AccumulationPassData data) {
    float fBaseAccumulation = params.fAccumulation;
    fBaseAccumulation = min(
        fBaseAccumulation,
        ffxLerp(fBaseAccumulation, 0.15,
                ffxSaturate(max(0.0, (params.f4KVelocity * VelocityFactor()) / 0.5))));
    data.fHistoryWeight = fBaseAccumulation;
}

float3 LoadPreparedColor(int2 iSamplePos) {
    float3 fRgb = max(float3(0.0, 0.0, 0.0), LoadInputColor(iSamplePos)) * Exposure();
    return RGBToYCoCg(fRgb);
}

// The sliced Lanczos filter of two lobes over the 4x4 render pixels around the display pixel's
// center, three by three of them toward it (the other slices gather over frames), with the box of
// their colors the history is clamped to.
void ComputeUpsampledColorAndWeight(AccumulationPassCommonParams params,
                                    inout AccumulationPassData data) {
    float2 fDstOutputPos = float2(params.iPxHrPos) + float2(0.5, 0.5);
    float2 fSrcOutputPos = fDstOutputPos * DownscaleFactor();
    int2 iSrcInputPos = int2(floor(fSrcOutputPos));
    // The unjittered position of the sample at offset (0, 0).
    float2 fSrcUnjitteredPos = (float2(iSrcInputPos) + float2(0.5, 0.5)) - Jitter();
    float2 fBaseSampleOffset = float2(fSrcUnjitteredPos - fSrcOutputPos);
    int2 offsetTL;
    offsetTL.x = (fSrcUnjitteredPos.x > fSrcOutputPos.x) ? -2 : -1;
    offsetTL.y = (fSrcUnjitteredPos.y > fSrcOutputPos.y) ? -2 : -1;
    bool bFlipRow = fSrcUnjitteredPos.y > fSrcOutputPos.y;
    bool bFlipCol = fSrcUnjitteredPos.x > fSrcOutputPos.x;
    float2 fOffsetTL = float2(offsetTL);
    bool bIsInitialSample = (params.fAccumulation == 0.0);

    float3 fSamples[9];
    int iSampleIndex = 0;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int2 iSampleColRow = int2(bFlipCol ? (3 - col) : col, bFlipRow ? (3 - row) : row);
            int2 iSrcSamplePos = iSrcInputPos + offsetTL + iSampleColRow;
            int2 iSampleCoord = ClampLoad(iSrcSamplePos, int2(0, 0), RenderSize());
            fSamples[iSampleIndex] = LoadPreparedColor(iSampleCoord);
            ++iSampleIndex;
        }
    }
    // HDR input: the first samples are tonemapped, through RGB so as not to desaturate them.
    if (bIsInitialSample) {
        for (iSampleIndex = 0; iSampleIndex < 9; ++iSampleIndex) {
            fSamples[iSampleIndex] = RGBToYCoCg(Tonemap(YCoCgToRGB(fSamples[iSampleIndex])));
        }
    }

    // How much of each upsampled color this frame takes: ComputeMaxKernelWeight and the rest.
    float fKernelBiasMax = min(1.99, 1.0 + (1.0 / DownscaleFactor().x - 1.0));
    float fKernelBiasMin = max(1.0, ((1.0 + fKernelBiasMax) * 0.3));
    float fKernelBiasWeight = min(1.0 - params.fDisocclusion * 0.5,
                                  min(1.0 - params.fShadingChange,
                                      ffxSaturate(data.fHistoryWeight * 5.0)));
    float fKernelBias = ffxLerp(fKernelBiasMin, fKernelBiasMax, fKernelBiasWeight);

    iSampleIndex = 0;
    for (int row2 = 0; row2 < 3; row2++) {
        for (int col2 = 0; col2 < 3; col2++) {
            int2 sampleColRow = int2(bFlipCol ? (3 - col2) : col2, bFlipRow ? (3 - row2) : row2);
            float2 fOffset = fOffsetTL + float2(sampleColRow);
            float2 fSrcSampleOffset = fBaseSampleOffset + fOffset;
            int2 iSrcSamplePos = iSrcInputPos + offsetTL + sampleColRow;
            float fOnScreenFactor = float(IsOnScreen(iSrcSamplePos, RenderSize()));
            if (!bIsInitialSample) {
                // GetUpsampleLanczosWeight with the approximate Lanczos.
                float2 fSrcSampleOffsetBiased = fSrcSampleOffset * fKernelBias;
                float fSampleWeight =
                    fOnScreenFactor *
                    Lanczos2ApproxSq(dot(fSrcSampleOffsetBiased, fSrcSampleOffsetBiased));
                data.fUpsampledColor += fSamples[iSampleIndex] * fSampleWeight;
                data.fUpsampledWeight += fSampleWeight;
            }
            // The rectification box.
            const float fRectificationCurveBias = -2.3;
            float fSrcSampleOffsetSq = dot(fSrcSampleOffset, fSrcSampleOffset);
            float fBoxSampleWeight =
                exp(fRectificationCurveBias * fSrcSampleOffsetSq) * fOnScreenFactor;
            bool bInitialSample = (row2 == 0) && (col2 == 0);
            RectificationBoxAddSample(bInitialSample, data.clippingBox, fSamples[iSampleIndex],
                                      fBoxSampleWeight);
            ++iSampleIndex;
        }
    }
    RectificationBoxComputeVarianceBoxData(data.clippingBox);

    data.fUpsampledWeight *= float(data.fUpsampledWeight > FSR3UPSCALER_EPSILON);
    if (data.fUpsampledWeight > FSR3UPSCALER_EPSILON) {
        // Normalized for deringing, which compares colors.
        data.fUpsampledColor = data.fUpsampledColor / data.fUpsampledWeight;
        data.fUpsampledWeight *= fAverageLanczosWeightPerFrame;
        data.fUpsampledColor =
            clamp(data.fUpsampledColor, data.clippingBox.aabbMin, data.clippingBox.aabbMax);
    }
    // The first samples, tonemapped.
    if (bIsInitialSample) {
        data.fUpsampledColor = RGBToYCoCg(InverseTonemap(YCoCgToRGB(data.clippingBox.boxCenter)));
        data.fUpsampledWeight = 1.0;
        data.fHistoryWeight = 0.0;
    }
}

void RectifyHistory(AccumulationPassCommonParams params, inout AccumulationPassData data) {
    float f4kVelocityFactor = ffxSaturate(params.f4KVelocity / 20.0);
    float fDistanceFactor = ffxSaturate(0.75 - params.fFarthestDepthInMeters / 20.0);
    float fAccumulationFactor = 1.0 - params.fAccumulation;
    float fReactiveFactor = ffxPow(params.fReactiveMask, 1.0 / 2.0);
    float fShadingChangeFactor = params.fShadingChange;
    float fBoxScaleT =
        max(f4kVelocityFactor,
            max(fDistanceFactor,
                max(fAccumulationFactor, max(fReactiveFactor, fShadingChangeFactor))));
    float fBoxScale = ffxLerp(3.0, 1.0, fBoxScaleT);
    float3 fScaledBoxVec = data.clippingBox.boxVec * float3(1.7, 1.0, 1.0) * fBoxScale;
    float3 fClampedScaledBoxVec = max(fScaledBoxVec, float3(1.193e-7, 1.193e-7, 1.193e-7));
    float3 fTransformedHistoryColor =
        (data.fHistoryColor - data.clippingBox.boxCenter) / fClampedScaledBoxVec;
    float fLength = sqrt(dot(fTransformedHistoryColor, fTransformedHistoryColor));
    if (fLength > 1.0) {
        float3 fClampedHistoryColor = fTransformedHistoryColor / fLength;
        float3 fFinalClampedHistoryColor =
            (fClampedHistoryColor * fScaledBoxVec) + data.clippingBox.boxCenter;
        // The history kept by how much of it is locked or flickering, where it has gathered
        // frames and wasn't hidden.
        float fHistoryContribution =
            max(params.fLumaInstabilityFactor, data.fLockContributionThisFrame) *
            params.fAccumulation * (1.0 - params.fDisocclusion);
        data.fHistoryColor = ffxLerp(fFinalClampedHistoryColor, data.fHistoryColor,
                                     ffxSaturate(fHistoryContribution));
    }
}

void Accumulate(inout AccumulationPassData data) {
    // No invalid values where the accumulation and upsampled weight are 0.
    data.fHistoryWeight *= float(data.fHistoryWeight > FSR3UPSCALER_FP16_MIN);
    data.fHistoryWeight = max(FSR3UPSCALER_EPSILON, data.fHistoryWeight + data.fUpsampledWeight);
    // HDR input: blended tonemapped, through RGB so as not to desaturate them.
    data.fUpsampledColor = RGBToYCoCg(Tonemap(YCoCgToRGB(data.fUpsampledColor)));
    data.fHistoryColor = RGBToYCoCg(Tonemap(YCoCgToRGB(data.fHistoryColor)));
    float fAlpha = ffxSaturate(data.fUpsampledWeight / data.fHistoryWeight);
    data.fHistoryColor = ffxLerp(data.fHistoryColor, data.fUpsampledColor, fAlpha);
    data.fHistoryColor = YCoCgToRGB(data.fHistoryColor);
    data.fHistoryColor = InverseTonemap(data.fHistoryColor);
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 iPxHrPos = int2(position.xy);
    AccumulationPassCommonParams params;
    AccumulationPassData data;
    InitPassData(iPxHrPos, params, data);
    if (params.bIsExistingSample && !params.bIsNewSample) {
        ReprojectHistoryColor(params, data);
    }
    UpdateLockStatus(params, data);
    ComputeBaseAccumulationWeight(params, data);
    ComputeUpsampledColorAndWeight(params, data);
    RectifyHistory(params, data);
    Accumulate(data);
    data.fHistoryColor /= Exposure();
    data.fHistoryColor = max(data.fHistoryColor, float3(0.0, 0.0, 0.0));
    return float4(data.fHistoryColor, data.fLock);
}

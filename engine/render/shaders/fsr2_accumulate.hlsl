// FSR 2's accumulate pass (ffx_fsr2_accumulate.h with ffx_fsr2_reproject.h, ffx_fsr2_upsample.h and
// ffx_fsr2_postprocess_lock_status.h): each display pixel's color, upsampled from the render
// pixels around it with a Lanczos kernel and blended into the frame before's, reprojected by the
// motion vectors, as far as the history holds up; and the locks and luma history the next frame
// reads. Keep it in step with src/fsr2.cpp, its CPU twin.
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

// The dilate pass's motion vectors, the depth clip pass's prepared color and reactive masks, the
// frame before's color, locks and luma history at the display size, the lock pass's new locks and
// the luminance pass's shading change mip.
Texture2D<float4> dilated_motion : register(t0, space2);
Texture2D<float4> prepared_color : register(t1, space2);
Texture2D<float4> reactive_masks : register(t2, space2);
Texture2D<float4> history_color : register(t3, space2);
Texture2D<float4> history_lock_status : register(t4, space2);
Texture2D<float4> new_locks : register(t5, space2);
Texture2D<float4> luma_mip : register(t6, space2);
Texture2D<float4> history_luma : register(t7, space2);
SamplerState dilated_motion_sampler : register(s0, space2);
SamplerState prepared_color_sampler : register(s1, space2);
SamplerState reactive_masks_sampler : register(s2, space2);
SamplerState history_color_sampler : register(s3, space2);
SamplerState history_lock_status_sampler : register(s4, space2);
SamplerState new_locks_sampler : register(s5, space2);
SamplerState luma_mip_sampler : register(s6, space2);
SamplerState history_luma_sampler : register(s7, space2);

float2 LoadDilatedMotionVector(int2 iPxPos) {
    return dilated_motion.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
float4 LoadPrepared(int2 iPxPos) {
    return prepared_color.Load(int3(Inside(iPxPos, RenderSize()), 0));
}
float3 LoadPreparedInputColor(int2 iPxPos) {
    return LoadPrepared(iPxPos).xyz;
}
float2 LoadReactiveMasks(int2 iPxPos) {
    return reactive_masks.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
float4 LoadHistory(int2 iPxPos) {
    return history_color.Load(int3(Inside(iPxPos, DisplaySize()), 0));
}
float4 LoadLockStatus(int2 iPxPos) {
    return float4(history_lock_status.Load(int3(Inside(iPxPos, DisplaySize()), 0)).xy, 0.0, 0.0);
}
float LoadRwNewLocks(int2 iPxPos) {
    return new_locks.Load(int3(Inside(iPxPos, DisplaySize()), 0)).r;
}
float LoadMipLuma(int2 iPxPos) {
    return luma_mip.Load(int3(Inside(iPxPos, LumaMipDimensions()), 0)).r;
}
float4 LoadLumaHistory(int2 iPxPos) {
    return history_luma.Load(int3(Inside(iPxPos, DisplaySize()), 0));
}

// The linear clamped samplers of AMD's callbacks, filtered in code.
float SampleDepthClip(float2 fUv) {
    int2 iBase;
    float2 fFrac = LinearWeights(fUv, RenderSize(), iBase);
    float fTop = ffxLerp(LoadPrepared(iBase).w, LoadPrepared(iBase + int2(1, 0)).w, fFrac.x);
    float fBottom =
        ffxLerp(LoadPrepared(iBase + int2(0, 1)).w, LoadPrepared(iBase + int2(1, 1)).w, fFrac.x);
    return ffxLerp(fTop, fBottom, fFrac.y);
}
float2 SampleDilatedReactiveMasks(float2 fUv) {
    int2 iBase;
    float2 fFrac = LinearWeights(fUv, RenderSize(), iBase);
    float2 fTop = ffxLerp(LoadReactiveMasks(iBase), LoadReactiveMasks(iBase + int2(1, 0)), fFrac.x);
    float2 fBottom = ffxLerp(LoadReactiveMasks(iBase + int2(0, 1)),
                             LoadReactiveMasks(iBase + int2(1, 1)), fFrac.x);
    return ffxLerp(fTop, fBottom, fFrac.y);
}
float SampleMipLuma(float2 fUv) {
    int2 iBase;
    float2 fFrac = LinearWeights(fUv, LumaMipDimensions(), iBase);
    float fTop = ffxLerp(LoadMipLuma(iBase), LoadMipLuma(iBase + int2(1, 0)), fFrac.x);
    float fBottom = ffxLerp(LoadMipLuma(iBase + int2(0, 1)), LoadMipLuma(iBase + int2(1, 1)), fFrac.x);
    return ffxLerp(fTop, fBottom, fFrac.y);
}
float4 SampleLumaHistory(float2 fUv) {
    int2 iBase;
    float2 fFrac = LinearWeights(fUv, DisplaySize(), iBase);
    float4 fTop = ffxLerp(LoadLumaHistory(iBase), LoadLumaHistory(iBase + int2(1, 0)), fFrac.x);
    float4 fBottom =
        ffxLerp(LoadLumaHistory(iBase + int2(0, 1)), LoadLumaHistory(iBase + int2(1, 1)), fFrac.x);
    return ffxLerp(fTop, fBottom, fFrac.y);
}

FSR2_LANCZOS2_SAMPLE(HistorySample, LoadHistory)
FSR2_BILINEAR_SAMPLE(LockStatusSample, LoadLockStatus)

float2 SampleLockStatus(float2 fUv) {
    return LockStatusSample(fUv, DisplaySize()).xy;
}

struct AccumulationPassCommonParams {
    int2 iPxHrPos;
    float2 fHrUv;
    float2 fLrUv_HwSampler;
    float2 fMotionVector;
    float2 fReprojectedHrUv;
    float fHrVelocity;
    float fDepthClipFactor;
    float fDilatedReactiveFactor;
    float fAccumulationMask;
    bool bIsResetFrame;
    bool bIsExistingSample;
    bool bIsNewSample;
};

struct LockState {
    bool NewLock;            // set for both unique new and re-locked new
    bool WasLockedPrevFrame; // set to identify if the pixel was already locked (relock)
};

void KillLock(inout float2 fLockStatus) {
    fLockStatus[LOCK_LIFETIME_REMAINING] = 0.0;
}

struct RectificationBox {
    float3 boxCenter;
    float3 boxVec;
    float3 aabbMin;
    float3 aabbMax;
    float fBoxCenterWeight;
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
    rectificationBox.fBoxCenterWeight = abs(rectificationBox.fBoxCenterWeight) > FSR2_EPSILON
                                            ? rectificationBox.fBoxCenterWeight
                                            : 1.0;
    rectificationBox.boxCenter /= rectificationBox.fBoxCenterWeight;
    rectificationBox.boxVec /= rectificationBox.fBoxCenterWeight;
    float3 stdDev =
        sqrt(abs(rectificationBox.boxVec - rectificationBox.boxCenter * rectificationBox.boxCenter));
    rectificationBox.boxVec = stdDev;
}

bool IsUvInside(float2 fUv) {
    return (fUv.x >= 0.0 && fUv.x <= 1.0) && (fUv.y >= 0.0 && fUv.y <= 1.0);
}

// ffx_fsr2_reproject.h

void ReprojectHistoryColor(AccumulationPassCommonParams params, out float3 fHistoryColor,
                           out float fTemporalReactiveFactor, out bool bInMotionLastFrame) {
    float4 fHistory = HistorySample(params.fReprojectedHrUv, DisplaySize());
    fHistoryColor = PrepareRgb(fHistory.rgb, Exposure(), PreviousFramePreExposure());
    fHistoryColor = RGBToYCoCg(fHistoryColor);
    fTemporalReactiveFactor = ffxSaturate(abs(fHistory.w));
    bInMotionLastFrame = (fHistory.w < 0.0);
}

LockState ReprojectHistoryLockStatus(AccumulationPassCommonParams params,
                                     out float2 fReprojectedLockStatus) {
    LockState state;
    state.NewLock = false;
    state.WasLockedPrevFrame = false;
    float fNewLockIntensity = LoadRwNewLocks(params.iPxHrPos);
    state.NewLock = fNewLockIntensity > (127.0 / 255.0);
    fReprojectedLockStatus = SampleLockStatus(params.fReprojectedHrUv);
    if (fReprojectedLockStatus[LOCK_LIFETIME_REMAINING] != 0.0) {
        state.WasLockedPrevFrame = true;
    }
    return state;
}

// ffx_fsr2_postprocess_lock_status.h

float GetShadingChangeLuma(int2 iPxHrPos, float2 fUvCoord) {
    int2 iMipRenderSize = int2(float2(RenderSize()) / 32.0);
    fUvCoord = ClampUv(fUvCoord, iMipRenderSize, LumaMipDimensions());
    float fShadingChangeLuma = Exposure() * exp(SampleMipLuma(fUvCoord));
    return ffxPow(fShadingChangeLuma, 1.0 / 6.0);
}

void UpdateLockStatus(AccumulationPassCommonParams params, inout float fReactiveFactor,
                      LockState state, inout float2 fLockStatus,
                      out float fLockContributionThisFrame, out float fLuminanceDiff) {
    float fShadingChangeLuma = GetShadingChangeLuma(params.iPxHrPos, params.fHrUv);
    fLockStatus[LOCK_TEMPORAL_LUMA] = (fLockStatus[LOCK_TEMPORAL_LUMA] == 0.0)
                                          ? fShadingChangeLuma
                                          : fLockStatus[LOCK_TEMPORAL_LUMA];
    float fPreviousShadingChangeLuma = fLockStatus[LOCK_TEMPORAL_LUMA];
    fLuminanceDiff = 1.0 - MinDividedByMax(fPreviousShadingChangeLuma, fShadingChangeLuma);
    if (state.NewLock) {
        fLockStatus[LOCK_TEMPORAL_LUMA] = fShadingChangeLuma;
        fLockStatus[LOCK_LIFETIME_REMAINING] =
            (fLockStatus[LOCK_LIFETIME_REMAINING] != 0.0) ? 2.0 : 1.0;
    } else if (fLockStatus[LOCK_LIFETIME_REMAINING] <= 1.0) {
        fLockStatus[LOCK_TEMPORAL_LUMA] =
            ffxLerp(fLockStatus[LOCK_TEMPORAL_LUMA], fShadingChangeLuma, 0.5);
    } else {
        if (fLuminanceDiff > 0.1) {
            KillLock(fLockStatus);
        }
    }
    fReactiveFactor = max(fReactiveFactor, ffxSaturate((fLuminanceDiff - 0.1) * 10.0));
    fLockStatus[LOCK_LIFETIME_REMAINING] *= (1.0 - fReactiveFactor);
    fLockStatus[LOCK_LIFETIME_REMAINING] *= ffxSaturate(1.0 - params.fAccumulationMask);
    fLockStatus[LOCK_LIFETIME_REMAINING] *= float(params.fDepthClipFactor < 0.1);
    float fLifetimeContribution = ffxSaturate(fLockStatus[LOCK_LIFETIME_REMAINING] - 1.0);
    float fShadingChangeContribution =
        ffxSaturate(MinDividedByMax(fLockStatus[LOCK_TEMPORAL_LUMA], fShadingChangeLuma));
    fLockContributionThisFrame =
        ffxSaturate(ffxSaturate(fLifetimeContribution * 4.0) * fShadingChangeContribution);
}

// ffx_fsr2_upsample.h

float GetUpsampleLanczosWeight(float2 fSrcSampleOffset, float fKernelWeight) {
    float2 fSrcSampleOffsetBiased = fSrcSampleOffset * fKernelWeight;
    return Lanczos2ApproxSq(dot(fSrcSampleOffsetBiased, fSrcSampleOffsetBiased));
}

float ComputeMaxKernelWeight() {
    const float fKernelSizeBias = 1.0;
    float fKernelWeight = 1.0 + (1.0 / DownscaleFactor() - 1.0).x * fKernelSizeBias;
    return min(1.99, fKernelWeight);
}

float4 ComputeUpsampledColorAndWeight(AccumulationPassCommonParams params,
                                      inout RectificationBox clippingBox, float fReactiveFactor) {
    // A sliced lanczos filter with 2 lobes (other slices are accumulated temporally).
    float2 fDstOutputPos = float2(params.iPxHrPos) + 0.5;
    float2 fSrcOutputPos = fDstOutputPos * DownscaleFactor();
    int2 iSrcInputPos = int2(floor(fSrcOutputPos));
    float3 fSamples[16];
    // The un-jittered position of the sample at offset 0,0.
    float2 fSrcUnjitteredPos = (float2(iSrcInputPos) + 0.5) - Jitter();
    int2 offsetTL;
    offsetTL.x = (fSrcUnjitteredPos.x > fSrcOutputPos.x) ? -2 : -1;
    offsetTL.y = (fSrcUnjitteredPos.y > fSrcOutputPos.y) ? -2 : -1;
    // Rows and columns flip where the offset is -2, so the first three of each make the box.
    bool bFlipRow = fSrcUnjitteredPos.y > fSrcOutputPos.y;
    bool bFlipCol = fSrcUnjitteredPos.x > fSrcOutputPos.x;
    float2 fOffsetTL = float2(offsetTL);
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int iSampleIndex = col + (row << 2);
            int2 sampleColRow = int2(bFlipCol ? (3 - col) : col, bFlipRow ? (3 - row) : row);
            int2 iSrcSamplePos = iSrcInputPos + offsetTL + sampleColRow;
            int2 sampleCoord = ClampLoad(iSrcSamplePos, int2(0, 0), RenderSize());
            fSamples[iSampleIndex] = LoadPreparedInputColor(sampleCoord);
        }
    }
    float4 fColorAndWeight = float4(0.0, 0.0, 0.0, 0.0);
    float2 fBaseSampleOffset = fSrcUnjitteredPos - fSrcOutputPos;
    // How much of each upsampled color this frame uses.
    float fKernelReactiveFactor = max(fReactiveFactor, float(params.bIsNewSample));
    float fKernelBiasMax = ComputeMaxKernelWeight() * (1.0 - fKernelReactiveFactor);
    float fKernelBiasMin = max(1.0, ((1.0 + fKernelBiasMax) * 0.3));
    float fKernelBiasFactor = max(0.0, max(0.25 * params.fDepthClipFactor, fKernelReactiveFactor));
    float fKernelBias = ffxLerp(fKernelBiasMax, fKernelBiasMin, fKernelBiasFactor);
    float fRectificationCurveBias = ffxLerp(-2.0, -3.0, ffxSaturate(params.fHrVelocity / 50.0));
    for (int row2 = 0; row2 < 3; row2++) {
        for (int col2 = 0; col2 < 3; col2++) {
            int iSampleIndex = col2 + (row2 << 2);
            int2 sampleColRow = int2(bFlipCol ? (3 - col2) : col2, bFlipRow ? (3 - row2) : row2);
            float2 fOffset = fOffsetTL + float2(sampleColRow);
            float2 fSrcSampleOffset = fBaseSampleOffset + fOffset;
            int2 iSrcSamplePos = iSrcInputPos + offsetTL + sampleColRow;
            float fOnScreenFactor = float(IsOnScreen(iSrcSamplePos, RenderSize()));
            float fSampleWeight =
                fOnScreenFactor * GetUpsampleLanczosWeight(fSrcSampleOffset, fKernelBias);
            fColorAndWeight += float4(fSamples[iSampleIndex] * fSampleWeight, fSampleWeight);
            // The rectification box.
            float fSrcSampleOffsetSq = dot(fSrcSampleOffset, fSrcSampleOffset);
            float fBoxSampleWeight = exp(fRectificationCurveBias * fSrcSampleOffsetSq);
            bool bInitialSample = (row2 == 0) && (col2 == 0);
            RectificationBoxAddSample(bInitialSample, clippingBox, fSamples[iSampleIndex],
                                      fBoxSampleWeight);
        }
    }
    RectificationBoxComputeVarianceBoxData(clippingBox);
    fColorAndWeight.w *= float(fColorAndWeight.w > FSR2_EPSILON);
    if (fColorAndWeight.w > FSR2_EPSILON) {
        // Normalized for deringing, which compares colors.
        fColorAndWeight.xyz = fColorAndWeight.xyz / fColorAndWeight.w;
        fColorAndWeight.w *= fUpsampleLanczosWeightScale;
        fColorAndWeight.xyz = clamp(fColorAndWeight.xyz, clippingBox.aabbMin, clippingBox.aabbMax);
    }
    return fColorAndWeight;
}

// ffx_fsr2_accumulate.h

void Accumulate(AccumulationPassCommonParams params, inout float3 fHistoryColor,
                float3 fAccumulation, float4 fUpsampledColorAndWeight) {
    // Avoids invalid values when the accumulation and upsampled weight are 0.
    fAccumulation = max(float3(FSR2_EPSILON, FSR2_EPSILON, FSR2_EPSILON),
                        fAccumulation + fUpsampledColorAndWeight.www);
    // YCoCg to RGB to tonemapped to YCoCg: the RGB tonemapper keeps colors from desaturating.
    fUpsampledColorAndWeight.xyz =
        RGBToYCoCg(Tonemap(YCoCgToRGB(fUpsampledColorAndWeight.xyz)));
    fHistoryColor = RGBToYCoCg(Tonemap(YCoCgToRGB(fHistoryColor)));
    float3 fAlpha = fUpsampledColorAndWeight.www / fAccumulation;
    fHistoryColor = ffxLerp(fHistoryColor, fUpsampledColorAndWeight.xyz, fAlpha);
    fHistoryColor = YCoCgToRGB(fHistoryColor);
    fHistoryColor = InverseTonemap(fHistoryColor);
}

void RectifyHistory(AccumulationPassCommonParams params, RectificationBox clippingBox,
                    inout float3 fHistoryColor, inout float3 fAccumulation,
                    float fLockContributionThisFrame, float fTemporalReactiveFactor,
                    float fLumaInstabilityFactor) {
    float fScaleFactorInfluence =
        min(20.0, ffxPow(1.0 / abs(DownscaleFactor().x * DownscaleFactor().y), 3.0));
    float fVecolityFactor = ffxSaturate(params.fHrVelocity / 20.0);
    float fBoxScaleT = max(params.fDepthClipFactor, max(params.fAccumulationMask, fVecolityFactor));
    float fBoxScale = ffxLerp(fScaleFactorInfluence, 1.0, fBoxScaleT);
    float3 fScaledBoxVec = clippingBox.boxVec * fBoxScale;
    float3 boxMin = clippingBox.boxCenter - fScaledBoxVec;
    float3 boxMax = clippingBox.boxCenter + fScaledBoxVec;
    boxMin = max(clippingBox.aabbMin, boxMin);
    boxMax = min(clippingBox.aabbMax, boxMax);
    if (any(boxMin > fHistoryColor) || any(fHistoryColor > boxMax)) {
        float3 fClampedHistoryColor = clamp(fHistoryColor, boxMin, boxMax);
        float fHistoryContribution = max(fLumaInstabilityFactor, fLockContributionThisFrame);
        float fReactiveFactor = params.fDilatedReactiveFactor;
        float fReactiveContribution = 1.0 - ffxPow(fReactiveFactor, 1.0 / 2.0);
        fHistoryContribution *= fReactiveContribution;
        // The history scaled by the rectification, the accumulation mask guarding against
        // invalid colors.
        fHistoryColor =
            ffxLerp(fClampedHistoryColor, fHistoryColor, ffxSaturate(fHistoryContribution));
        // The accumulation scaled by the rectification.
        float3 fAccumulationMin = min(fAccumulation, float3(0.1, 0.1, 0.1));
        fAccumulation = ffxLerp(fAccumulationMin, fAccumulation, ffxSaturate(fHistoryContribution));
    }
}

float2 FinalizeLockStatus(AccumulationPassCommonParams params, float2 fLockStatus,
                          float fUpsampledWeight) {
    // Expecting similar motion next frame, a lock that will be off screen dies, so locks aren't
    // clamped to the screen's edges.
    float2 fEstimatedUvNextFrame = params.fHrUv - params.fMotionVector;
    if (!IsUvInside(fEstimatedUvNextFrame)) {
        KillLock(fLockStatus);
    } else {
        // The lock's lifetime decreases.
        float fLifetimeDecreaseLanczosMax = JitterSequenceLength() * fAverageLanczosWeightPerFrame;
        float fLifetimeDecrease = fUpsampledWeight / fLifetimeDecreaseLanczosMax;
        fLockStatus[LOCK_LIFETIME_REMAINING] =
            max(0.0, fLockStatus[LOCK_LIFETIME_REMAINING] - fLifetimeDecrease);
    }
    return fLockStatus;
}

float3 ComputeBaseAccumulationWeight(AccumulationPassCommonParams params,
                                     float fThisFrameReactiveFactor, bool bInMotionLastFrame,
                                     float fUpsampledWeight) {
    // Max accumulation is always taken as reached.
    float fBaseAccumulation = fMaxAccumulationLanczosWeight * float(params.bIsExistingSample) *
                              (1.0 - fThisFrameReactiveFactor) * (1.0 - params.fDepthClipFactor);
    fBaseAccumulation = min(fBaseAccumulation,
                            ffxLerp(fBaseAccumulation, fUpsampledWeight * 10.0,
                                    max(float(bInMotionLastFrame),
                                        ffxSaturate(params.fHrVelocity * 10.0))));
    fBaseAccumulation =
        min(fBaseAccumulation, ffxLerp(fBaseAccumulation, fUpsampledWeight,
                                       ffxSaturate(params.fHrVelocity / 20.0)));
    return float3(fBaseAccumulation, fBaseAccumulation, fBaseAccumulation);
}

float ComputeLumaInstabilityFactor(AccumulationPassCommonParams params,
                                   RectificationBox clippingBox, float fThisFrameReactiveFactor,
                                   float fLuminanceDiff, out float4 fLumaHistoryOut) {
    const float fUnormThreshold = 1.0 / 255.0;
    const int N_MINUS_1 = 0;
    const int N_MINUS_2 = 1;
    const int N_MINUS_3 = 2;
    const int N_MINUS_4 = 3;
    float fCurrentFrameLuma = clippingBox.boxCenter.x;
    fCurrentFrameLuma = fCurrentFrameLuma / (1.0 + max(0.0, fCurrentFrameLuma));
    fCurrentFrameLuma = ffxRound(fCurrentFrameLuma * 255.0) / 255.0;
    bool bSampleLumaHistory =
        (max(max(params.fDepthClipFactor, params.fAccumulationMask), fLuminanceDiff) < 0.1) &&
        (params.bIsNewSample == false);
    float4 fCurrentFrameLumaHistory = bSampleLumaHistory
                                          ? SampleLumaHistory(params.fReprojectedHrUv)
                                          : float4(0.0, 0.0, 0.0, 0.0);
    float fLumaInstability = 0.0;
    float fDiffs0 = (fCurrentFrameLuma - fCurrentFrameLumaHistory[N_MINUS_1]);
    float fMin = abs(fDiffs0);
    if (fMin >= fUnormThreshold) {
        for (int i = N_MINUS_2; i <= N_MINUS_4; i++) {
            float fDiffs1 = (fCurrentFrameLuma - fCurrentFrameLumaHistory[i]);
            if (sign(fDiffs0) == sign(fDiffs1)) {
                // The difference scaled to protect historically similar values.
                const float fMinBias = 1.0;
                fMin = min(fMin, abs(fDiffs1) * fMinBias);
            }
        }
        float fBoxSize = clippingBox.boxVec.x;
        float fBoxSizeFactor = ffxPow(ffxSaturate(fBoxSize / 0.1), 6.0);
        fLumaInstability = float(fMin != abs(fDiffs0)) * fBoxSizeFactor;
        fLumaInstability = float(fLumaInstability > fUnormThreshold);
        fLumaInstability *=
            1.0 - max(params.fAccumulationMask, ffxPow(fThisFrameReactiveFactor, 1.0 / 6.0));
    }
    // The history shifts.
    fCurrentFrameLumaHistory[N_MINUS_4] = fCurrentFrameLumaHistory[N_MINUS_3];
    fCurrentFrameLumaHistory[N_MINUS_3] = fCurrentFrameLumaHistory[N_MINUS_2];
    fCurrentFrameLumaHistory[N_MINUS_2] = fCurrentFrameLumaHistory[N_MINUS_1];
    fCurrentFrameLumaHistory[N_MINUS_1] = fCurrentFrameLuma;
    fLumaHistoryOut = fCurrentFrameLumaHistory;
    return fLumaInstability * float(fCurrentFrameLumaHistory[N_MINUS_4] != 0.0);
}

float ComputeTemporalReactiveFactor(AccumulationPassCommonParams params,
                                    float fTemporalReactiveFactor) {
    float fNewFactor = min(0.99, fTemporalReactiveFactor);
    fNewFactor = max(fNewFactor, ffxLerp(fNewFactor, 0.4, ffxSaturate(params.fHrVelocity)));
    fNewFactor = max(fNewFactor * fNewFactor,
                     max(params.fDepthClipFactor * 0.1, params.fDilatedReactiveFactor));
    // New samples are fully reactive.
    fNewFactor = params.bIsNewSample ? 1.0 : fNewFactor;
    if (ffxSaturate(params.fHrVelocity * 10.0) >= 1.0) {
        fNewFactor = max(FSR2_EPSILON, fNewFactor) * -1.0;
    }
    return fNewFactor;
}

AccumulationPassCommonParams InitParams(int2 iPxHrPos) {
    AccumulationPassCommonParams params;
    params.iPxHrPos = iPxHrPos;
    float2 fHrUv = (float2(iPxHrPos) + 0.5) / float2(DisplaySize());
    params.fHrUv = fHrUv;
    float2 fLrUvJittered = fHrUv + Jitter() / float2(RenderSize());
    params.fLrUv_HwSampler = ClampUv(fLrUvJittered, RenderSize(), RenderSize());
    // Low resolution motion vectors, as Godot gives FSR 2.
    params.fMotionVector = LoadDilatedMotionVector(int2(fHrUv * float2(RenderSize())));
    params.fHrVelocity = length(params.fMotionVector * float2(DisplaySize()));
    params.fReprojectedHrUv = params.fHrUv + params.fMotionVector;
    params.bIsExistingSample = IsUvInside(params.fReprojectedHrUv);
    params.fDepthClipFactor = ffxSaturate(SampleDepthClip(params.fLrUv_HwSampler));
    float2 fDilatedReactiveMasks = SampleDilatedReactiveMasks(params.fLrUv_HwSampler);
    params.fDilatedReactiveFactor = fDilatedReactiveMasks.x;
    params.fAccumulationMask = fDilatedReactiveMasks.y;
    params.bIsResetFrame = (0 == FrameIndex());
    params.bIsNewSample = (params.bIsExistingSample == false || params.bIsResetFrame);
    return params;
}

struct Accumulated {
    float4 color : SV_Target0;       // the internal color and temporal reactive factor
    float4 lock_status : SV_Target1; // the lock's remaining lifetime and temporal luma
    float4 luma_history : SV_Target2;
};

Accumulated fs_main(float4 position : SV_Position) {
    int2 iPxHrPos = int2(position.xy);
    AccumulationPassCommonParams params = InitParams(iPxHrPos);
    float3 fHistoryColor = float3(0.0, 0.0, 0.0);
    float2 fLockStatus = float2(0.0, 0.0);
    float fTemporalReactiveFactor = 0.0;
    bool bInMotionLastFrame = false;
    LockState lockState;
    lockState.NewLock = false;
    lockState.WasLockedPrevFrame = false;
    if (params.bIsExistingSample && !params.bIsResetFrame) {
        ReprojectHistoryColor(params, fHistoryColor, fTemporalReactiveFactor, bInMotionLastFrame);
        lockState = ReprojectHistoryLockStatus(params, fLockStatus);
    }
    float fThisFrameReactiveFactor = max(params.fDilatedReactiveFactor, fTemporalReactiveFactor);
    float fLuminanceDiff = 0.0;
    float fLockContributionThisFrame = 0.0;
    UpdateLockStatus(params, fThisFrameReactiveFactor, lockState, fLockStatus,
                     fLockContributionThisFrame, fLuminanceDiff);
    // The upsampled input color.
    RectificationBox clippingBox;
    clippingBox.boxCenter = float3(0.0, 0.0, 0.0);
    clippingBox.boxVec = float3(0.0, 0.0, 0.0);
    clippingBox.aabbMin = float3(FSR2_FLT_MAX, FSR2_FLT_MAX, FSR2_FLT_MAX);
    clippingBox.aabbMax = -float3(FSR2_FLT_MAX, FSR2_FLT_MAX, FSR2_FLT_MAX);
    clippingBox.fBoxCenterWeight = 0.0;
    float4 fUpsampledColorAndWeight =
        ComputeUpsampledColorAndWeight(params, clippingBox, fThisFrameReactiveFactor);
    Accumulated result;
    float fLumaInstabilityFactor = ComputeLumaInstabilityFactor(
        params, clippingBox, fThisFrameReactiveFactor, fLuminanceDiff, result.luma_history);
    float3 fAccumulation = ComputeBaseAccumulationWeight(
        params, fThisFrameReactiveFactor, bInMotionLastFrame, fUpsampledColorAndWeight.w);
    if (params.bIsNewSample) {
        fHistoryColor = YCoCgToRGB(fUpsampledColorAndWeight.xyz);
    } else {
        RectifyHistory(params, clippingBox, fHistoryColor, fAccumulation,
                       fLockContributionThisFrame, fThisFrameReactiveFactor,
                       fLumaInstabilityFactor);
        Accumulate(params, fHistoryColor, fAccumulation, fUpsampledColorAndWeight);
    }
    fHistoryColor = UnprepareRgb(fHistoryColor, Exposure());
    fLockStatus = FinalizeLockStatus(params, fLockStatus, fUpsampledColorAndWeight.w);
    // The new temporal reactive factor.
    fTemporalReactiveFactor = ComputeTemporalReactiveFactor(params, fThisFrameReactiveFactor);
    result.color = float4(fHistoryColor, fTemporalReactiveFactor);
    result.lock_status = float4(fLockStatus, 0.0, 0.0);
    return result;
}

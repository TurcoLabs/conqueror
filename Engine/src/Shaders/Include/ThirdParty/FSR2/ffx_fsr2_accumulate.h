// This file is part of the FidelityFX SDK.
//
// Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef FFX_FSR2_ACCUMULATE_H
#define FFX_FSR2_ACCUMULATE_H

// Conqueror Engine change (354): a short lighting change that the renderer reports (a muzzle flash light, at most 6 frames) is not
// written into the history, as in the engine's TAA and TAAU (338): at a 1:1 ratio with a still camera the history weight is high and
// thin details are only clipped by the variance box, so the flash frame stayed in the tile lines of a ceiling and faded over ~1.5 s.
// In those frames an existing, still, non-disoccluded, non-reactive pixel that is not on an object moving on its own (r_cq_freeze_mask)
// keeps its reprojected history, lock and luma history unchanged, while the output shows the frame exactly as before (the flash
// included). Showing the frozen history plus the light's low-frequency contribution instead left outlines where frozen and normal
// pixels met (smoke puffs, the flame's quad, the weapon's vacated area)
#if defined(FFX_GLSL)
layout (set = 1, binding = 60, std140) uniform cbCq_t
{
    FfxInt32 iCqLightingFreeze;
    FfxInt32 iCqPad0;
    FfxInt32 iCqPad1;
    FfxInt32 iCqPad2;
} cbCq;
layout (set = 1, binding = 61) uniform texture2D r_cq_freeze_mask;

// How much of the pixel is frozen (0..1), smooth in the reactive and depth clip factors and in the motion. A sprite of this frame
// (flame, smoke) takes the normal path: frozen under it, the display had to show the raw sprite pixels and its outline (with the
// ceiling's lines aliased inside it) stayed visible for a few frames. A moving pixel (the camera kicked by recoil) is not frozen: its
// history would only be resampled for those frames, softening the floor's lines afterwards; with motion the history weight is low
// and the flash fades quickly anyway
FfxFloat32 CqFreezeWeight(const AccumulationPassCommonParams params)
{
    if (cbCq.iCqLightingFreeze == 0 || !params.bIsExistingSample || params.bIsResetFrame)
        return 0.0f;
    const FfxInt32x2 iRenderPos = FfxInt32x2(params.fHrUv * FfxFloat32x2(RenderSize()));
    if (texelFetch(r_cq_freeze_mask, iRenderPos, 0).r > 0.5f)
        return 0.0f;
    return (1.0f - smoothstep(0.02f, 0.3f, params.fDilatedReactiveFactor)) * (1.0f - smoothstep(0.02f, 0.3f, params.fDepthClipFactor))
         * (1.0f - smoothstep(0.02f, 0.1f, params.fHrVelocity));
}
FfxBoolean CqLightingFreezeFrame()
{
    return cbCq.iCqLightingFreeze != 0;
}
#else
FfxFloat32 CqFreezeWeight(const AccumulationPassCommonParams params)
{
    return 0.0f;
}
FfxBoolean CqLightingFreezeFrame()
{
    return FFX_FALSE;
}
#endif


FfxFloat32 GetPxHrVelocity(FfxFloat32x2 fMotionVector)
{
    return length(fMotionVector * DisplaySize());
}
#if FFX_HALF
FFX_MIN16_F GetPxHrVelocity(FFX_MIN16_F2 fMotionVector)
{
    return length(fMotionVector * FFX_MIN16_F2(DisplaySize()));
}
#endif

void Accumulate(const AccumulationPassCommonParams params, FFX_PARAMETER_INOUT FfxFloat32x3 fHistoryColor, FfxFloat32x3 fAccumulation, FFX_PARAMETER_IN FfxFloat32x4 fUpsampledColorAndWeight)
{
    // Aviod invalid values when accumulation and upsampled weight is 0
    fAccumulation = ffxMax(FSR2_EPSILON.xxx, fAccumulation + fUpsampledColorAndWeight.www);

#if FFX_FSR2_OPTION_HDR_COLOR_INPUT
    //YCoCg -> RGB -> Tonemap -> YCoCg (Use RGB tonemapper to avoid color desaturation)
    fUpsampledColorAndWeight.xyz = RGBToYCoCg(Tonemap(YCoCgToRGB(fUpsampledColorAndWeight.xyz)));
    fHistoryColor = RGBToYCoCg(Tonemap(YCoCgToRGB(fHistoryColor)));
#endif

    const FfxFloat32x3 fAlpha = fUpsampledColorAndWeight.www / fAccumulation;
    fHistoryColor = ffxLerp(fHistoryColor, fUpsampledColorAndWeight.xyz, fAlpha);

    fHistoryColor = YCoCgToRGB(fHistoryColor);

#if FFX_FSR2_OPTION_HDR_COLOR_INPUT
    fHistoryColor = InverseTonemap(fHistoryColor);
#endif
}

void RectifyHistory(
    const AccumulationPassCommonParams params,
    RectificationBox clippingBox,
    FFX_PARAMETER_INOUT FfxFloat32x3 fHistoryColor,
    FFX_PARAMETER_INOUT FfxFloat32x3 fAccumulation,
    FfxFloat32 fLockContributionThisFrame,
    FfxFloat32 fTemporalReactiveFactor,
    FfxFloat32 fLumaInstabilityFactor)
{
    FfxFloat32 fScaleFactorInfluence = ffxMin(20.0f, ffxPow(FfxFloat32(1.0f / length(DownscaleFactor().x * DownscaleFactor().y)), 3.0f));
    // Conqueror Engine change (native AA): the box scale above is 1 (tightest) at 1:1, so sub-pixel lines missed by the current
    // jitter sample were clipped out of the history every few frames. At ratios near 1:1 the box is widened to 3 while still;
    // velocity, depth clip and the accumulation mask shrink it back to 1 exactly as before
    // Completely still (zero motion): 6, so a sub-pixel line that the current jitter sample misses stays in the history
    const FfxFloat32 fNativeBox = ffxSaturate((DownscaleFactor().x - 0.7f) / 0.3f);
    fScaleFactorInfluence = ffxMax(fScaleFactorInfluence, ffxLerp(ffxLerp(1.0f, 3.0f, fNativeBox), 6.0f, fNativeBox * FfxFloat32(params.fHrVelocity < 0.001f)));

    const FfxFloat32 fVecolityFactor = ffxSaturate(params.fHrVelocity / 20.0f);
    const FfxFloat32 fBoxScaleT = ffxMax(params.fDepthClipFactor, ffxMax(params.fAccumulationMask, fVecolityFactor));
    FfxFloat32 fBoxScale = ffxLerp(fScaleFactorInfluence, 1.0f, fBoxScaleT);

    FfxFloat32x3 fScaledBoxVec = clippingBox.boxVec * fBoxScale;
    FfxFloat32x3 boxMin = clippingBox.boxCenter - fScaledBoxVec;
    FfxFloat32x3 boxMax = clippingBox.boxCenter + fScaledBoxVec;
    FfxFloat32x3 boxCenter = clippingBox.boxCenter;
    FfxFloat32 boxVecSize = length(clippingBox.boxVec);

    // Conqueror Engine change (native AA, still camera): the variance box was always cut to the min/max of this frame's 3x3
    // samples, so whatever box scale was used, a sub-pixel line missed by the current jitter sample fell outside and was
    // clipped (and the accumulation dropped to 0.1). With zero motion near 1:1 only the variance box is used: in flat areas it
    // stays tight (light changes are still clipped), at thin features it keeps the line in the history
    // Moving near 1:1 the cut caused the same loss every frame (sub-pixel gaps blinked while walking): it now fades in with
    // velocity only (fully back from 20 px / frame), depth clip and the accumulation mask, like the box scale itself
    const FfxFloat32 fStillNative = fNativeBox * (1.0f - fBoxScaleT);
    boxMin = ffxLerp(ffxMax(clippingBox.aabbMin, boxMin), boxMin, fStillNative);
    boxMax = ffxLerp(ffxMin(clippingBox.aabbMax, boxMax), boxMax, fStillNative);

    if (any(FFX_GREATER_THAN(boxMin, fHistoryColor)) || any(FFX_GREATER_THAN(fHistoryColor, boxMax))) {

        const FfxFloat32x3 fClampedHistoryColor = clamp(fHistoryColor, boxMin, boxMax);

        FfxFloat32x3 fHistoryContribution = ffxMax(fLumaInstabilityFactor, fLockContributionThisFrame).xxx;
        
        const FfxFloat32 fReactiveFactor = params.fDilatedReactiveFactor;
        const FfxFloat32 fReactiveContribution = 1.0f - ffxPow(fReactiveFactor, 1.0f / 2.0f);
        fHistoryContribution *= fReactiveContribution;

        // Scale history color using rectification info, also using accumulation mask to avoid potential invalid color protection
        fHistoryColor = ffxLerp(fClampedHistoryColor, fHistoryColor, ffxSaturate(fHistoryContribution));

        // Scale accumulation using rectification info
        const FfxFloat32x3 fAccumulationMin = ffxMin(fAccumulation, FFX_BROADCAST_FLOAT32X3(0.1f));
        // Conqueror Engine change (native AA, moving): every clip dropped the accumulation to 0.1, so in the frames whose jitter
        // missed a sub-pixel gap the gap was clipped and replaced by this frame's sample almost completely: the gaps between the
        // cubes blinked while walking. Near 1:1, and while moving at upscaling ratios, a small excursion (a sub-pixel detail, < 0.04 in YCoCg) keeps the accumulation
        // (the history is still clipped, as in TAA); a large one (a tracer or flash that just vanished, a missed disocclusion)
        // still drops it, so new and vanished bright objects are not dimmed or smeared
        const FfxFloat32 fExcursion = length(fHistoryColor - fClampedHistoryColor);
        const FfxFloat32 fNativeKeep = ffxLerp(ffxSaturate(params.fHrVelocity * 10.0f), 1.0f, ffxSaturate((DownscaleFactor().x - 0.7f) / 0.3f)) * (1.0f - ffxMax(params.fDepthClipFactor, params.fAccumulationMask)) * (1.0f - smoothstep(0.04f, 0.12f, fExcursion));
        fAccumulation = ffxLerp(fAccumulationMin, fAccumulation, ffxSaturate(ffxMax(fHistoryContribution, FFX_BROADCAST_FLOAT32X3(fNativeKeep))));
    }
}

void WriteUpscaledOutput(FfxInt32x2 iPxHrPos, FfxFloat32x3 fUpscaledColor)
{
    StoreUpscaledOutput(iPxHrPos, fUpscaledColor);
}

void FinalizeLockStatus(const AccumulationPassCommonParams params, FfxFloat32x2 fLockStatus, FfxFloat32 fUpsampledWeight)
{
    // we expect similar motion for next frame
    // kill lock if that location is outside screen, avoid locks to be clamped to screen borders
    FfxFloat32x2 fEstimatedUvNextFrame = params.fHrUv - params.fMotionVector;
    if (IsUvInside(fEstimatedUvNextFrame) == false) {
        KillLock(fLockStatus);
    }
    else {
        // Decrease lock lifetime
        const FfxFloat32 fLifetimeDecreaseLanczosMax = FfxFloat32(JitterSequenceLength()) * FfxFloat32(fAverageLanczosWeightPerFrame);
        const FfxFloat32 fLifetimeDecrease = FfxFloat32(fUpsampledWeight / fLifetimeDecreaseLanczosMax);
        fLockStatus[LOCK_LIFETIME_REMAINING] = ffxMax(FfxFloat32(0), fLockStatus[LOCK_LIFETIME_REMAINING] - fLifetimeDecrease);
    }

    StoreLockStatus(params.iPxHrPos, fLockStatus);
}


FfxFloat32x3 ComputeBaseAccumulationWeight(const AccumulationPassCommonParams params, FfxFloat32 fThisFrameReactiveFactor, FfxBoolean bInMotionLastFrame, FfxFloat32 fUpsampledWeight, LockState lockState, FfxFloat32 fLumaInstabilityFactor, FfxFloat32 fStillFrames)
{
    // Always assume max accumulation was reached
    // Conqueror Engine change (native AA): at a 1:1 render/display ratio every output pixel gets a current-frame sample within
    // half a pixel, its Lanczos weight is ~1, so a history weight of 1 blended 50% of each jittered frame in and the jitter
    // pattern stayed visible on thin features even with a still camera. The history weight rises to 8 as the ratio approaches
    // 1:1; upscaling ratios (render <= 70% of display) keep the SDK value. Motion still lowers it below (velocity terms)
    const FfxFloat32 fNativeT = ffxSaturate((DownscaleFactor().x - 0.7f) / 0.3f);
    // With a completely still camera (zero motion) the history weight near 1:1 rises further, to 24, as in a still TAA; a
    // light change still clips the history in RectifyHistory, which then drops the accumulation, so flashes are not dimmed
    const FfxFloat32 fStillT = fNativeT * FfxFloat32(params.fHrVelocity < 0.001f);
    // Conqueror Engine change (native AA, still camera): a sample weighs about 0.06 here, so a fixed history weight of 24 blends 0.25% of
    // each frame and the image took 10 seconds to settle after moving (the gaps between the cubes slowly grew darker). The history weight
    // now follows the samples gathered since the pixel stopped: an average of those frames, 15% of the new frame at most
    const FfxFloat32 fStillBase = ffxMin(24.0f, fUpsampledWeight * ffxMax(fStillFrames, 5.67f));
    FfxFloat32 fBaseAccumulation = ffxLerp(ffxLerp(fMaxAccumulationLanczosWeight, 8.0f, fNativeT), fStillBase, fStillT) * FfxFloat32(params.bIsExistingSample) * (1.0f - fThisFrameReactiveFactor) * (1.0f - params.fDepthClipFactor);

    fBaseAccumulation = ffxMin(fBaseAccumulation, ffxLerp(fBaseAccumulation, fUpsampledWeight * 10.0f, ffxMax(FfxFloat32(bInMotionLastFrame), ffxSaturate(params.fHrVelocity * FfxFloat32(10)))));

    fBaseAccumulation = ffxMin(fBaseAccumulation, ffxLerp(fBaseAccumulation, fUpsampledWeight, ffxSaturate(params.fHrVelocity / FfxFloat32(20))));

    // Conqueror Engine change: a flickering pixel (luma instability: a sub-pixel detail drawn only in some jitter frames, its history
    // kept unclipped) keeps a high history weight, like TAA's flicker hold, so the held detail averages over the whole jitter cycle
    // instead of following each frame (16 near 1:1; 4 at upscaling ratios, where the history is denser, and only while moving)
    const FfxFloat32 fHoldT = ffxLerp(ffxSaturate(params.fHrVelocity * 10.0f), 1.0f, ffxSaturate((DownscaleFactor().x - 0.7f) / 0.3f)) * fLumaInstabilityFactor * FfxFloat32(params.bIsExistingSample) * (1.0f - params.fDepthClipFactor) * (1.0f - params.fAccumulationMask);
    fBaseAccumulation = ffxMax(fBaseAccumulation, ffxLerp(4.0f, 16.0f, ffxSaturate((DownscaleFactor().x - 0.7f) / 0.3f)) * fHoldT);
    return fBaseAccumulation.xxx;
}

FfxFloat32 ComputeLumaInstabilityFactor(const AccumulationPassCommonParams params, RectificationBox clippingBox, FfxFloat32 fThisFrameReactiveFactor, FfxFloat32 fLuminanceDiff, FfxBoolean bCqFreeze)
{
    const FfxFloat32 fUnormThreshold = 1.0f / 255.0f;
    const FfxInt32 N_MINUS_1 = 0;
    const FfxInt32 N_MINUS_2 = 1;
    const FfxInt32 N_MINUS_3 = 2;
    const FfxInt32 N_MINUS_4 = 3;

    FfxFloat32 fCurrentFrameLuma = clippingBox.boxCenter.x;

#if FFX_FSR2_OPTION_HDR_COLOR_INPUT
    fCurrentFrameLuma = fCurrentFrameLuma / (1.0f + ffxMax(0.0f, fCurrentFrameLuma));
#endif

    fCurrentFrameLuma = round(fCurrentFrameLuma * 255.0f) / 255.0f;

    const FfxBoolean bSampleLumaHistory = (ffxMax(ffxMax(params.fDepthClipFactor, params.fAccumulationMask), fLuminanceDiff) < 0.1f) && (params.bIsNewSample == false);
    FfxFloat32x4 fCurrentFrameLumaHistory = bSampleLumaHistory ? SampleLumaHistory(params.fReprojectedHrUv) : FFX_BROADCAST_FLOAT32X4(0.0f);

    FfxFloat32 fLumaInstability = 0.0f;
    FfxFloat32 fDiffs0 = (fCurrentFrameLuma - fCurrentFrameLumaHistory[N_MINUS_1]);

    FfxFloat32 fMin = abs(fDiffs0);

    if (fMin >= fUnormThreshold)
    {
        for (int i = N_MINUS_2; i <= N_MINUS_4; i++) {
            FfxFloat32 fDiffs1 = (fCurrentFrameLuma - fCurrentFrameLumaHistory[i]);

            if (sign(fDiffs0) == sign(fDiffs1)) {
                
                // Scale difference to protect historically similar values
                const FfxFloat32 fMinBias = 1.0f;
                fMin = ffxMin(fMin, abs(fDiffs1) * fMinBias);
            }
        }

        const FfxFloat32 fBoxSize = clippingBox.boxVec.x;
        const FfxFloat32 fBoxSizeFactor = ffxPow(ffxSaturate(fBoxSize / 0.1f), 6.0f);

        fLumaInstability = FfxFloat32(fMin != abs(fDiffs0)) * fBoxSizeFactor;
        // Conqueror Engine change (native AA): the box size factor above only protects high contrast neighbourhoods, but in the
        // frames whose jitter misses a sub-pixel gap the 3x3 box is flat and the gap was clipped away (blinking while walking).
        // Near 1:1 (at upscaling ratios only while moving) a pixel also counts as unstable when at least two of the last four frames differ from this one in the same
        // direction (the detail keeps coming and going) and the change is small (< 0.08: a tracer or muzzle flash that lit the
        // pixel for one frame differs a lot, in one frame only, and is not held)
        FfxInt32 iDiffer = 0;
        for (int i = N_MINUS_1; i <= N_MINUS_4; i++) {
            const FfxFloat32 fD = fCurrentFrameLuma - fCurrentFrameLumaHistory[i];
            iDiffer += (sign(fD) == sign(fDiffs0) && abs(fD) > 2.0f * fUnormThreshold) ? 1 : 0;
        }
        const FfxFloat32 fNativeFlicker = ffxLerp(ffxSaturate(params.fHrVelocity * 10.0f), 1.0f, ffxSaturate((DownscaleFactor().x - 0.7f) / 0.3f)) * FfxFloat32(fMin != abs(fDiffs0)) * FfxFloat32(iDiffer >= 2) * FfxFloat32(abs(fDiffs0) < 0.08f);
        fLumaInstability = ffxMax(fLumaInstability, fNativeFlicker);
        fLumaInstability = FfxFloat32(fLumaInstability > fUnormThreshold);

        fLumaInstability *= 1.0f - ffxMax(params.fAccumulationMask, ffxPow(fThisFrameReactiveFactor, 1.0f / 6.0f));
    }

    //shift history
    fCurrentFrameLumaHistory[N_MINUS_4] = fCurrentFrameLumaHistory[N_MINUS_3];
    fCurrentFrameLumaHistory[N_MINUS_3] = fCurrentFrameLumaHistory[N_MINUS_2];
    fCurrentFrameLumaHistory[N_MINUS_2] = fCurrentFrameLumaHistory[N_MINUS_1];
    fCurrentFrameLumaHistory[N_MINUS_1] = fCurrentFrameLuma;

    // Conqueror Engine change (354): a frozen pixel keeps its luma history (the flash frame is not a flicker)
    if (!bCqFreeze) {
        StoreLumaHistory(params.iPxHrPos, fCurrentFrameLumaHistory);
    }

    return fLumaInstability * FfxFloat32(fCurrentFrameLumaHistory[N_MINUS_4] != 0);
}

FfxFloat32 ComputeTemporalReactiveFactor(const AccumulationPassCommonParams params, FfxFloat32 fTemporalReactiveFactor)
{
    FfxFloat32 fNewFactor = ffxMin(0.99f, fTemporalReactiveFactor);

    // Conqueror Engine change: from 1 px / frame of motion every pixel became 0.4 reactive, which cut the history weight, the lock
    // lifetime and the instability protection of thin details on every moving frame. The motion term is off at every ratio; moving
    // objects and shadows keep their reactive masks and depth clip
    fNewFactor = ffxMax(fNewFactor, ffxLerp(fNewFactor, 0.0f, ffxSaturate(params.fHrVelocity)));

    fNewFactor = ffxMax(fNewFactor * fNewFactor, ffxMax(params.fDepthClipFactor * 0.1f, params.fDilatedReactiveFactor));

    // Force reactive factor for new samples
    fNewFactor = params.bIsNewSample ? 1.0f : fNewFactor;

    if (ffxSaturate(params.fHrVelocity * 10.0f) >= 1.0f) {
        fNewFactor = ffxMax(FSR2_EPSILON, fNewFactor) * -1.0f;
    }
    
    return fNewFactor;
}

AccumulationPassCommonParams InitParams(FfxInt32x2 iPxHrPos)
{
    AccumulationPassCommonParams params;

    params.iPxHrPos = iPxHrPos;
    const FfxFloat32x2 fHrUv = (iPxHrPos + 0.5f) / DisplaySize();
    params.fHrUv = fHrUv;
    
    const FfxFloat32x2 fLrUvJittered = fHrUv + Jitter() / RenderSize();
    params.fLrUv_HwSampler = ClampUv(fLrUvJittered, RenderSize(), MaxRenderSize());

    params.fMotionVector = GetMotionVector(iPxHrPos, fHrUv);
    params.fHrVelocity = GetPxHrVelocity(params.fMotionVector);

    ComputeReprojectedUVs(params, params.fReprojectedHrUv, params.bIsExistingSample);

    params.fDepthClipFactor = ffxSaturate(SampleDepthClip(params.fLrUv_HwSampler));
    
    const FfxFloat32x2 fDilatedReactiveMasks = SampleDilatedReactiveMasks(params.fLrUv_HwSampler);
    params.fDilatedReactiveFactor = fDilatedReactiveMasks.x;
    params.fAccumulationMask = fDilatedReactiveMasks.y;
    params.bIsResetFrame = (0 == FrameIndex());

    params.bIsNewSample = (params.bIsExistingSample == false || params.bIsResetFrame);

    return params;
}

void Accumulate(FfxInt32x2 iPxHrPos)
{
    const AccumulationPassCommonParams params = InitParams(iPxHrPos);

    FfxFloat32x3 fHistoryColor = FfxFloat32x3(0, 0, 0);
    FfxFloat32x2 fLockStatus;
    InitializeNewLockSample(fLockStatus);

    FfxFloat32 fTemporalReactiveFactor = 0.0f;
    FfxBoolean bInMotionLastFrame = FFX_FALSE;
    FfxFloat32 fStillFrames = 0.0f;
    LockState lockState = { FFX_FALSE , FFX_FALSE };
    if (params.bIsExistingSample && !params.bIsResetFrame) {
        ReprojectHistoryColor(params, fHistoryColor, fTemporalReactiveFactor, bInMotionLastFrame, fStillFrames);
        lockState = ReprojectHistoryLockStatus(params, fLockStatus);
    }

    FfxFloat32 fThisFrameReactiveFactor = ffxMax(params.fDilatedReactiveFactor, fTemporalReactiveFactor);

    // A history that was reactive in the last frames (a flame or tracer sprite that is gone now) is not frozen: freezing kept the
    // vanished sprite in the history, and its outline stayed on the wall for several frames
    // Read without filtering, so that the still-frame count survives exactly
    const FfxFloat32 fCqReprojectedAlpha = LoadHistory(clamp(FfxInt32x2(params.fReprojectedHrUv * FfxFloat32x2(DisplaySize())), FfxInt32x2(0, 0), FfxInt32x2(DisplaySize()) - 1)).w;
    // The temporal reactive fraction of the history pixel (read without filtering: a filtered read mixes the still-frame counts of
    // neighbouring pixels into the fraction). In RGBA16F it keeps only strong recent reactivity once a pixel has been still for a while
    // (0.25 steps above 256 still frames), which is what is wanted here: thin smoke that drifted over the surface does not stop it from
    // freezing, a sprite that covered the pixel a frame ago does
    FfxFloat32 fCqHistoryReactive = 0.0f;
    if (CqLightingFreezeFrame() && params.bIsExistingSample) {
        const FfxFloat32 fEncoded = abs(fCqReprojectedAlpha);
        fCqHistoryReactive = ffxSaturate(fEncoded - floor(fEncoded));
    }
    const FfxFloat32 fCqFreeze = CqFreezeWeight(params) * (1.0f - smoothstep(0.005f, 0.02f, fCqHistoryReactive));
    // Locks, luma history and the stored alpha follow the dominant side; the colour is blended
    const FfxBoolean bCqFreeze = fCqFreeze > 0.5f;
    const FfxFloat32x3 fCqFrozenColor = YCoCgToRGB(fHistoryColor);
    // Conqueror Engine change (354): the alpha the history had, stored back unchanged for a frozen pixel
    const FfxFloat32x2 fCqLockStatusBefore = fLockStatus;

    FfxFloat32 fLuminanceDiff = 0.0f;
    FfxFloat32 fLockContributionThisFrame = 0.0f;
    UpdateLockStatus(params, fThisFrameReactiveFactor, lockState, fLockStatus, fLockContributionThisFrame, fLuminanceDiff);
    if (bCqFreeze) {
        fLockStatus = fCqLockStatusBefore;
    }

    // Load upsampled input color
    RectificationBox clippingBox;
    FfxFloat32x4 fUpsampledColorAndWeight = ComputeUpsampledColorAndWeight(params, clippingBox, fThisFrameReactiveFactor);
    
    const FfxFloat32 fLumaInstabilityFactor = ComputeLumaInstabilityFactor(params, clippingBox, fThisFrameReactiveFactor, fLuminanceDiff, bCqFreeze);


    FfxFloat32x3 fAccumulation = ComputeBaseAccumulationWeight(params, fThisFrameReactiveFactor, bInMotionLastFrame, fUpsampledColorAndWeight.w, lockState, fLumaInstabilityFactor, fStillFrames);

    if (params.bIsNewSample) {
        fHistoryColor = YCoCgToRGB(fUpsampledColorAndWeight.xyz);
    }
    else {
        RectifyHistory(params, clippingBox, fHistoryColor, fAccumulation, fLockContributionThisFrame, fThisFrameReactiveFactor, fLumaInstabilityFactor);

        Accumulate(params, fHistoryColor, fAccumulation, fUpsampledColorAndWeight);
    }
    // Conqueror Engine change (354): shown as accumulated; stored with the reprojected history unchanged (no rectification, no blend)
    // as far as the pixel is frozen
    FfxFloat32x3 fCqOutputColor = fHistoryColor;
    if (!params.bIsNewSample) {
        fHistoryColor = ffxLerp(fHistoryColor, fCqFrozenColor, fCqFreeze);
    }

    fHistoryColor = UnprepareRgb(fHistoryColor, Exposure());
    fCqOutputColor = UnprepareRgb(fCqOutputColor, Exposure());

    FinalizeLockStatus(params, fLockStatus, fUpsampledColorAndWeight.w);

    // Get new temporal reactive factor
    fTemporalReactiveFactor = ComputeTemporalReactiveFactor(params, fThisFrameReactiveFactor);

    // Conqueror Engine change: alpha = sign (in motion) x (still frames + reactive factor), see ReprojectHistoryColor
    const FfxFloat32 fStillNow = FfxFloat32(params.fHrVelocity < 0.001f && DownscaleFactor().x > 0.7f) * ffxMin(fStillFrames + 1.0f, 400.0f);
    FfxFloat32 fStoredAlpha = (fTemporalReactiveFactor < 0.0f ? -1.0f : 1.0f) * (ffxMin(abs(fTemporalReactiveFactor), 0.99f) + fStillNow);
    if (bCqFreeze) {
        fStoredAlpha = fCqReprojectedAlpha;   // frozen: the history's alpha unchanged
    }
    StoreInternalColorAndWeight(iPxHrPos, FfxFloat32x4(fHistoryColor, fStoredAlpha));

    // Output final color when RCAS is disabled
#if FFX_FSR2_OPTION_APPLY_SHARPENING == 0
    WriteUpscaledOutput(iPxHrPos, fCqOutputColor);
#endif
    StoreNewLocks(iPxHrPos, 0);
}

#endif // FFX_FSR2_ACCUMULATE_H

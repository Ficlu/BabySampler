// chromagram.c
//
// HPCP with Instantaneous Frequency (IF) refinement.
//
// Standard peak-picking + parabolic interpolation gives ±1-2 Hz
// frequency error per peak. At 65 Hz (C2), a semitone is only
// 3.7 Hz wide, so this error causes frequent misattribution.
//
// Phase-derivative IF estimation solves this: for each spectral
// peak, we compare the FFT phase between the current and previous
// frame. A pure sinusoid at frequency f causes the phase of bin k
// (center frequency f_k) to advance by exactly:
//
//     expected_advance = 2π × k × H / N     (if f == f_k)
//
// Any deviation from this expected advance reveals the true frequency:
//
//     IF[k] = f_k + unwrap(actual_advance - expected_advance) × Fs / (2π × H)
//
// For an isolated sinusoid, this gives the EXACT frequency regardless
// of FFT size. Even at 55 Hz with an 8192-point FFT (5.9 Hz/bin),
// IF estimation achieves sub-Hz precision.
//
// References:
//   - Müller, "Fundamentals of Music Processing" (2015), §8.2.1
//   - Kodera, Gendrin & de Villedary, Phys. Earth Planet. Inter. (1976)
//   - Auger & Flandrin, IEEE TSP (1995)
//   - Gomez, "Tonal Description of Music Audio Signals" (2006)

#include "chromagram.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Minimum peak magnitude relative to the frame's strongest peak.
#define HPCP_RELATIVE_THRESHOLD 0.01f

// ---- Radix-2 Cooley-Tukey FFT ----

static int BitReverse(int x, int numBits)
{
    int result = 0;
    for (int i = 0; i < numBits; i++) {
        result = (result << 1) | (x & 1);
        x >>= 1;
    }
    return result;
}

static void FFT(float *data, int N)
{
    int numBits = 0;
    int temp = N;
    while (temp > 1) { temp >>= 1; numBits++; }

    for (int i = 0; i < N; i++) {
        int j = BitReverse(i, numBits);
        if (j > i) {
            float tr = data[2*i], ti = data[2*i+1];
            data[2*i] = data[2*j]; data[2*i+1] = data[2*j+1];
            data[2*j] = tr; data[2*j+1] = ti;
        }
    }

    for (int size = 2; size <= N; size *= 2) {
        int halfSize = size / 2;
        float angleStep = -2.0f * (float)M_PI / (float)size;
        for (int i = 0; i < N; i += size) {
            for (int j = 0; j < halfSize; j++) {
                float angle = angleStep * (float)j;
                float wr = cosf(angle), wi = sinf(angle);
                int evenIdx = 2*(i+j), oddIdx = 2*(i+j+halfSize);
                float tr = wr*data[oddIdx] - wi*data[oddIdx+1];
                float ti = wr*data[oddIdx+1] + wi*data[oddIdx];
                data[oddIdx] = data[evenIdx] - tr;
                data[oddIdx+1] = data[evenIdx+1] - ti;
                data[evenIdx] += tr;
                data[evenIdx+1] += ti;
            }
        }
    }
}

// ---- Principal argument: wrap to [-π, π] ----

static float PrincipalArg(float phase)
{
    // Fast wrap to [-π, π]
    while (phase > (float)M_PI)  phase -= 2.0f * (float)M_PI;
    while (phase < -(float)M_PI) phase += 2.0f * (float)M_PI;
    return phase;
}

// ---- Context management ----

BOOL ChromagramContext_Init(ChromagramContext *ctx, int fftSize, int hopSize)
{
    if (!ctx || fftSize < 2) return FALSE;
    int specSize = fftSize / 2;

    ctx->prevReal = (float *)calloc(specSize, sizeof(float));
    ctx->prevImag = (float *)calloc(specSize, sizeof(float));
    if (!ctx->prevReal || !ctx->prevImag) {
        if (ctx->prevReal) free(ctx->prevReal);
        if (ctx->prevImag) free(ctx->prevImag);
        ctx->prevReal = NULL;
        ctx->prevImag = NULL;
        return FALSE;
    }

    ctx->specSize = specSize;
    ctx->hopSize = hopSize;
    ctx->hasPrev = FALSE;
    return TRUE;
}

void ChromagramContext_Free(ChromagramContext *ctx)
{
    if (!ctx) return;
    if (ctx->prevReal) { free(ctx->prevReal); ctx->prevReal = NULL; }
    if (ctx->prevImag) { free(ctx->prevImag); ctx->prevImag = NULL; }
    ctx->hasPrev = FALSE;
}

// ---- HPCP with IF refinement ----

Chromagram ComputeHPCP(ChromagramContext *ctx,
                       const float *samples, int sampleCount, DWORD sampleRate)
{
    Chromagram result;
    memset(&result, 0, sizeof(Chromagram));
    if (!samples || sampleCount < 2) return result;

    float sr = (float)sampleRate;
    float freqRes = sr / (float)sampleCount;
    int N = sampleCount;

    float *fftData = (float *)malloc(2 * N * sizeof(float));
    if (!fftData) return result;

    // Blackman-Harris window: -92dB sidelobes
    float Nm1 = (float)(N - 1);
    for (int i = 0; i < N; i++) {
        float n = (float)i;
        float w = 0.35875f
                - 0.48829f * cosf(2.0f * (float)M_PI * n / Nm1)
                + 0.14128f * cosf(4.0f * (float)M_PI * n / Nm1)
                - 0.01168f * cosf(6.0f * (float)M_PI * n / Nm1);
        fftData[2*i]   = samples[i] * w;
        fftData[2*i+1] = 0.0f;
    }

    FFT(fftData, N);

    int specSize = N / 2;
    float *mag = (float *)malloc(specSize * sizeof(float));
    if (!mag) { free(fftData); return result; }

    // Extract magnitude and store complex values for IF
    float maxMag = 0.0f;
    for (int i = 0; i < specSize; i++) {
        float re = fftData[2*i], im = fftData[2*i+1];
        mag[i] = sqrtf(re*re + im*im);
        if (mag[i] > maxMag) maxMag = mag[i];
    }

    // Two-level thresholding
    float magSum = 0.0f;
    for (int i = 0; i < specSize; i++) magSum += mag[i];
    float noiseFloor = (magSum / (float)specSize) * 0.1f;
    float relFloor = maxMag * HPCP_RELATIVE_THRESHOLD;
    float threshold = noiseFloor > relFloor ? noiseFloor : relFloor;

    // Precompute: can we use IF estimation this frame?
    BOOL useIF = (ctx != NULL && ctx->hasPrev &&
                  ctx->specSize == specSize && ctx->hopSize > 0);

    // Expected phase advance per bin per hop (radians)
    float phaseAdvancePerBin = 0.0f;
    if (useIF) {
        phaseAdvancePerBin = 2.0f * (float)M_PI * (float)ctx->hopSize / (float)N;
    }

    // Search range
    int minBin = (int)(HPCP_MIN_FUNDAMENTAL_FREQ / freqRes);
    int maxBin = (int)(HPCP_MAX_FUNDAMENTAL_FREQ / freqRes);
    if (minBin < 1) minBin = 1;
    if (maxBin >= specSize - 1) maxBin = specSize - 2;

    for (int bin = minBin; bin <= maxBin; bin++) {
        if (mag[bin] <= threshold) continue;
        if (mag[bin] < mag[bin-1] || mag[bin] < mag[bin+1]) continue;

        float freq;
        float peakMag = mag[bin];

        if (useIF) {
            // ---- Instantaneous Frequency estimation ----
            //
            // Phase of current frame at this bin
            float curReal = fftData[2*bin];
            float curImag = fftData[2*bin+1];
            float curPhase = atan2f(curImag, curReal);

            // Phase of previous frame at this bin
            float prevPhase = atan2f(ctx->prevImag[bin], ctx->prevReal[bin]);

            // Actual phase advance
            float actualAdvance = curPhase - prevPhase;

            // Expected phase advance for a sinusoid at the bin center
            float expectedAdvance = (float)bin * phaseAdvancePerBin;

            // Phase deviation: the difference reveals the true frequency
            float deviation = PrincipalArg(actualAdvance - expectedAdvance);

            // Convert deviation to frequency offset
            // deviation (radians) / (2π × H/Fs) = frequency offset (Hz)
            float freqOffset = deviation * sr / (2.0f * (float)M_PI * (float)ctx->hopSize);

            freq = (float)bin * freqRes + freqOffset;

            // Sanity check: IF estimate should be within ±0.5 bins of peak
            // (wider tolerance than parabolic because IF works at all frequencies)
            float binFreq = (float)bin * freqRes;
            if (freq < binFreq - 0.6f * freqRes || freq > binFreq + 0.6f * freqRes) {
                // IF estimate unreliable (multi-component bin) — fall back
                freq = binFreq;
                float alpha = mag[bin-1], beta = mag[bin], gamma = mag[bin+1];
                float denom = alpha - 2.0f*beta + gamma;
                if (fabsf(denom) > 0.0001f) {
                    float p = 0.5f * (alpha - gamma) / denom;
                    if (fabsf(p) < 1.0f) {
                        freq = ((float)bin + p) * freqRes;
                        peakMag = beta - 0.25f * (alpha - gamma) * p;
                    }
                }
            }
        } else {
            // ---- Parabolic interpolation fallback (first frame) ----
            freq = (float)bin * freqRes;
            float alpha = mag[bin-1], beta = mag[bin], gamma = mag[bin+1];
            float denom = alpha - 2.0f*beta + gamma;
            if (fabsf(denom) > 0.0001f) {
                float p = 0.5f * (alpha - gamma) / denom;
                if (fabsf(p) < 1.0f) {
                    freq = ((float)bin + p) * freqRes;
                    peakMag = beta - 0.25f * (alpha - gamma) * p;
                }
            }
        }

        // Reject peaks outside detection range after interpolation
        if (freq < HPCP_MIN_FUNDAMENTAL_FREQ || freq > HPCP_MAX_FUNDAMENTAL_FREQ)
            continue;

        // Squared magnitude (energy)
        float energy = peakMag * peakMag;

        // Pitch class mapping
        float semi = 12.0f * log2f(freq / HPCP_REF_FREQ);
        int pc = (((int)roundf(semi) + 9) % 12 + 12) % 12;

        result.bins[pc] += energy;
        result.totalEnergy += energy;
    }

    // Store current FFT for next frame's IF calculation
    if (ctx != NULL && ctx->specSize == specSize) {
        for (int i = 0; i < specSize; i++) {
            ctx->prevReal[i] = fftData[2*i];
            ctx->prevImag[i] = fftData[2*i+1];
        }
        ctx->hasPrev = TRUE;
    }

    free(mag);
    free(fftData);
    return result;
}
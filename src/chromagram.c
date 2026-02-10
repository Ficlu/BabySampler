// chromagram.c
//
// HPCP with Instantaneous Frequency (IF) refinement and harmonic folding.
//
// Two refinements over basic HPCP:
//
// 1. IF estimation (Kodera 1976, Auger & Flandrin 1995):
//    For each spectral peak, we compare FFT phase between frames.
//    A pure sinusoid at frequency f causes bin k's phase to advance by:
//        expected = 2π × k × H / N
//    Deviation from this reveals the true frequency:
//        IF[k] = f_k + unwrap(actual - expected) × Fs / (2π × H)
//    This gives sub-Hz precision at all frequencies.
//
// 2. Harmonic folding (Gómez 2006):
//    Each spectral peak at frequency f might be:
//      - A fundamental at f         (h=1, weight 1.0)
//      - The 2nd harmonic of f/2    (h=2, weight λ)
//      - The 3rd harmonic of f/3    (h=3, weight λ²)
//      - ...
//    We distribute each peak's energy across all candidate fundamentals
//    with exponential decay λ^(h-1). This folds harmonic series energy
//    back toward the fundamental pitch class.
//
//    Why this matters: a C3 note with harmonics produces peaks at C3,
//    C4, G4, C5, E5, G5, Bb5... Without folding, G4 and E5 pollute
//    the G and E pitch classes. With folding, these peaks attribute
//    most of their energy back to C (via h=3 and h=5 hypotheses).
//
// References:
//   - Gómez, "Tonal Description of Music Audio Signals" (2006), §3.2
//   - Müller, "Fundamentals of Music Processing" (2015), §8.2.1
//   - Kodera, Gendrin & de Villedary, Phys. Earth Planet. Inter. (1976)
//   - Auger & Flandrin, IEEE TSP (1995)

#include "chromagram.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// ---- HPCP with IF refinement and harmonic folding ----

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

    // Extract magnitude
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

    // ---- Precompute harmonic weights ----
    //
    // harmonicWeight[h] = λ^h  (note: array is 0-indexed, h=0 means harmonic 1)
    // harmonicWeight[0] = 1.0 (fundamental)
    // harmonicWeight[1] = λ   (2nd harmonic)
    // harmonicWeight[2] = λ²  (3rd harmonic)
    // ...
    float harmonicWeight[HPCP_MAX_HARMONIC];
    harmonicWeight[0] = 1.0f;
    for (int h = 1; h < HPCP_MAX_HARMONIC; h++) {
        harmonicWeight[h] = harmonicWeight[h - 1] * HPCP_HARMONIC_DECAY;
    }

    // ---- Peak search range ----
    //
    // minBin: lowest frequency we care about as a fundamental.
    // maxBin: highest frequency at which we search for peaks.
    //
    // We scan up to HPCP_MAX_PEAK_FREQ (not HPCP_MAX_FUNDAMENTAL_FREQ)
    // because peaks above the fundamental range can still be harmonics
    // of notes within the fundamental range.
    //
    // Example: a peak at 3000 Hz could be the 3rd harmonic of A5 (880 Hz)
    // → candidate fundamental at 1000 Hz → maps to pitch class.
    // Without extending the range, we'd miss this harmonic energy entirely.
    int minBin = (int)(HPCP_MIN_FUNDAMENTAL_FREQ / freqRes);
    int maxBin = (int)(HPCP_MAX_PEAK_FREQ / freqRes);
    if (minBin < 1) minBin = 1;
    if (maxBin >= specSize - 1) maxBin = specSize - 2;

    for (int bin = minBin; bin <= maxBin; bin++) {
        if (mag[bin] <= threshold) continue;
        if (mag[bin] < mag[bin-1] || mag[bin] < mag[bin+1]) continue;

        float freq;
        float peakMag = mag[bin];

        if (useIF) {
            // ---- Instantaneous Frequency estimation ----
            float curReal = fftData[2*bin];
            float curImag = fftData[2*bin+1];
            float curPhase = atan2f(curImag, curReal);

            float prevPhase = atan2f(ctx->prevImag[bin], ctx->prevReal[bin]);

            float actualAdvance = curPhase - prevPhase;
            float expectedAdvance = (float)bin * phaseAdvancePerBin;
            float deviation = PrincipalArg(actualAdvance - expectedAdvance);
            float freqOffset = deviation * sr / (2.0f * (float)M_PI * (float)ctx->hopSize);

            freq = (float)bin * freqRes + freqOffset;

            // Sanity check: IF estimate should be within ±0.6 bins of peak
            float binFreq = (float)bin * freqRes;
            if (freq < binFreq - 0.6f * freqRes || freq > binFreq + 0.6f * freqRes) {
                // IF estimate unreliable — fall back to parabolic
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

        // Reject peaks outside the extended detection range.
        // Peaks below MIN_FUNDAMENTAL can't be a valid fundamental or
        // a useful harmonic of anything in range. Peaks above MAX_PEAK
        // are beyond where we expect musically relevant harmonics.
        if (freq < HPCP_MIN_FUNDAMENTAL_FREQ || freq > HPCP_MAX_PEAK_FREQ)
            continue;

        // Squared magnitude (energy) for this peak
        float energy = peakMag * peakMag;

        // ---- Harmonic folding (Gómez 2006, §3.2) ----
        //
        // For this peak at frequency `freq`, test each harmonic hypothesis:
        //
        //   h=1: this peak IS a fundamental at freq        → weight 1.0
        //   h=2: this peak is harmonic 2 of fund at freq/2 → weight λ
        //   h=3: this peak is harmonic 3 of fund at freq/3 → weight λ²
        //   ...
        //
        // Each hypothesis produces a candidate fundamental frequency.
        // If that candidate falls within the valid fundamental range,
        // we map it to a pitch class and add weighted energy.
        //
        // The result: if a C3 note produces harmonic peaks at G4 (3rd),
        // E5 (5th), etc., those peaks contribute energy back to pitch
        // class C with weights 0.36 and 0.13 respectively. Without
        // folding, they'd pollute G and E pitch classes instead.
        //
        // Note: candidateFreq decreases as h increases, so once it
        // drops below MIN_FUNDAMENTAL we can stop (all higher h values
        // will be even lower).

        for (int h = 0; h < HPCP_MAX_HARMONIC; h++) {
            float candidateFreq = freq / (float)(h + 1);

            // Candidate below valid range — done (further h only go lower)
            if (candidateFreq < HPCP_MIN_FUNDAMENTAL_FREQ)
                break;

            // Candidate above valid fundamental range — skip this h,
            // but lower h values (higher divisor) may bring it in range.
            // This happens when the peak is between MAX_FUNDAMENTAL and
            // MAX_PEAK — it only contributes via h >= 2 hypotheses.
            if (candidateFreq > HPCP_MAX_FUNDAMENTAL_FREQ)
                continue;

            // Map candidate fundamental to pitch class
            //
            // semi = number of semitones from A4 (440 Hz)
            // pc = pitch class 0-11 where C=0, C#=1, ..., B=11
            //
            // The +9 shifts from A-relative to C-relative:
            //   A is 0 semitones from A4, and A = pitch class 9
            //   so (0 + 9) % 12 = 9 ✓
            //   C is -9 semitones from A4 (or +3), (3 + 9) % 12 = 0 ✓
            float semi = 12.0f * log2f(candidateFreq / HPCP_REF_FREQ);
            int pc = (((int)roundf(semi) + 9) % 12 + 12) % 12;

            float contribution = harmonicWeight[h] * energy;
            result.bins[pc] += contribution;
            result.totalEnergy += contribution;
        }
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
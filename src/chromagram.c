// chromagram.c
//
// HPCP with three refinements:
//   1. Instantaneous Frequency via phase derivative (sub-Hz accuracy)
//   2. Harmonic folding (attributes harmonics back to fundamentals)
//   3. Spectral whitening (suppresses sidelobe false peaks)
//
// Pipeline:
//   FFT → magnitude spectrum → spectral envelope (moving average)
//   → whitened spectrum (mag / envelope) → peak detection on whitened
//   → IF refinement using raw phase → harmonic folding using raw energy
//   → pitch class accumulation
//
// Whitening only affects WHICH bins are identified as peaks.
// Energy calculation uses raw magnitudes so the chromagram reflects
// actual signal energy, not the normalized values.
//
// References:
//   - Gómez, "Tonal Description of Music Audio Signals" (2006)
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

    // Initialize tuning estimation state
    memset(ctx->tuningHistogram, 0, sizeof(ctx->tuningHistogram));
    ctx->tuningPeakCount = 0;
    ctx->tuningOffsetCents = 0.0f;
    ctx->tuningLocked = FALSE;

    return TRUE;
}

void ChromagramContext_Free(ChromagramContext *ctx)
{
    if (!ctx) return;
    if (ctx->prevReal) { free(ctx->prevReal); ctx->prevReal = NULL; }
    if (ctx->prevImag) { free(ctx->prevImag); ctx->prevImag = NULL; }
    ctx->hasPrev = FALSE;
}

// ---- Tuning estimation ----
//
// Finds the global tuning offset by analyzing the distribution of
// fractional-semitone offsets across all detected spectral peaks.
//
// Approach: each peak's frequency maps to some fractional semitone
// value. The fractional part (how far the peak is from the nearest
// integer semitone) reveals the tuning offset. If the source is
// tuned to A=442, every peak will be ~+8 cents sharp, and the
// histogram of offsets will peak at +8.
//
// We smooth the histogram to suppress noise, then find its maximum.
// The result is the estimated tuning offset in cents.
//
// Ref: Gomez (2006) section 3.1; Essentia TuningFrequency algorithm

float EstimateTuning(ChromagramContext *ctx)
{
    if (!ctx || ctx->tuningPeakCount < HPCP_TUNING_MIN_PEAKS) {
        return 0.0f;  // Not enough data — assume 440 Hz
    }

    // Smooth the tuning histogram with a moving average
    float smoothed[HPCP_TUNING_BINS];
    for (int i = 0; i < HPCP_TUNING_BINS; i++) {
        float sum = 0.0f;
        int count = 0;
        for (int j = -HPCP_TUNING_SMOOTH; j <= HPCP_TUNING_SMOOTH; j++) {
            int idx = i + j;
            if (idx >= 0 && idx < HPCP_TUNING_BINS) {
                sum += ctx->tuningHistogram[idx];
                count++;
            }
        }
        smoothed[i] = sum / (float)count;
    }

    // Find the peak of the smoothed histogram
    int bestBin = 50;  // center = 0 cents offset
    float bestVal = smoothed[50];
    for (int i = 0; i < HPCP_TUNING_BINS; i++) {
        if (smoothed[i] > bestVal) {
            bestVal = smoothed[i];
            bestBin = i;
        }
    }

    // Parabolic interpolation for sub-cent accuracy
    float offsetCents = (float)(bestBin - 50);  // raw bin to cents
    if (bestBin > 0 && bestBin < HPCP_TUNING_BINS - 1) {
        float s0 = smoothed[bestBin - 1];
        float s1 = smoothed[bestBin];
        float s2 = smoothed[bestBin + 1];
        float denom = 2.0f * (2.0f * s1 - s0 - s2);
        if (fabsf(denom) > 0.0001f) {
            float delta = (s0 - s2) / denom;
            if (fabsf(delta) < 1.0f) {
                offsetCents += delta;
            }
        }
    }

    // Commit the result
    ctx->tuningOffsetCents = offsetCents;
    ctx->tuningLocked = TRUE;

    return offsetCents;
}

// ---- HPCP with IF refinement, harmonic folding, and spectral whitening ----

Chromagram ComputeHPCP(ChromagramContext *ctx,
                       const float *samples, int sampleCount, DWORD sampleRate)
{
    Chromagram result;
    memset(&result, 0, sizeof(Chromagram));
    if (!samples || sampleCount < 2) return result;

    float sr = (float)sampleRate;
    float freqRes = sr / (float)sampleCount;
    int N = sampleCount;

    // Use tuning-corrected reference frequency if available.
    // Before tuning is locked, we use the default 440 Hz and accumulate
    // offset data. Once locked, we shift the reference to compensate.
    float refFreq = HPCP_REF_FREQ;
    if (ctx != NULL && ctx->tuningLocked) {
        refFreq = HPCP_REF_FREQ * powf(2.0f, ctx->tuningOffsetCents / 1200.0f);
    }

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

    // Allocate working arrays: raw magnitude, envelope, whitened magnitude
    float *mag      = (float *)malloc(specSize * sizeof(float));
    float *envelope  = (float *)malloc(specSize * sizeof(float));
    float *whitened  = (float *)malloc(specSize * sizeof(float));
    if (!mag || !envelope || !whitened) {
        free(fftData);
        if (mag) free(mag);
        if (envelope) free(envelope);
        if (whitened) free(whitened);
        return result;
    }

    // ---- Extract raw magnitudes ----
    float maxMag = 0.0f;
    for (int i = 0; i < specSize; i++) {
        float re = fftData[2*i], im = fftData[2*i+1];
        mag[i] = sqrtf(re*re + im*im);
        if (mag[i] > maxMag) maxMag = mag[i];
    }

    // ---- Spectral whitening: compute moving-average envelope ----
    //
    // For each bin i, envelope[i] = mean(mag[i-W .. i+W]) where W is
    // HPCP_WHITEN_HALF_WINDOW. We use a running sum for O(N) efficiency.
    //
    // Why moving average rather than moving median:
    //   - O(N) vs O(N·W·log W) computational cost
    //   - A single peak in an 81-bin window contributes ~10% to the mean,
    //     so it doesn't inflate its own envelope much
    //   - For our purposes (sidelobe suppression), this is sufficient;
    //     moving median would be slightly more robust but not worth the cost
    {
        int halfWin = HPCP_WHITEN_HALF_WINDOW;
        float runSum = 0.0f;
        int winCount = 0;

        // Seed: sum elements [0 .. halfWin]
        for (int i = 0; i <= halfWin && i < specSize; i++) {
            runSum += mag[i];
            winCount++;
        }
        envelope[0] = runSum / (float)winCount;

        for (int i = 1; i < specSize; i++) {
            // Add new element entering window on the right
            int addIdx = i + halfWin;
            if (addIdx < specSize) {
                runSum += mag[addIdx];
                winCount++;
            }
            // Remove element leaving window on the left
            int removeIdx = i - halfWin - 1;
            if (removeIdx >= 0) {
                runSum -= mag[removeIdx];
                winCount--;
            }
            envelope[i] = runSum / (float)winCount;
        }
    }

    // ---- Compute whitened magnitudes ----
    //
    // whitened[i] = mag[i] / envelope[i]
    //
    // A genuine spectral peak will have whitened value >> 1.0 (it stands
    // well above the local average). A sidelobe or noise floor bin will
    // have whitened value ≈ 1.0 (it's at the local average level).
    //
    // The floor prevents division by zero in silent regions. We use a
    // tiny fraction of the global max so the floor scales with signal level.
    {
        float envFloor = maxMag * 1e-6f;
        if (envFloor < 1e-20f) envFloor = 1e-20f;

        for (int i = 0; i < specSize; i++) {
            float env = envelope[i] > envFloor ? envelope[i] : envFloor;
            whitened[i] = mag[i] / env;
        }
    }

    // ---- Raw magnitude thresholds (unchanged from before) ----
    //
    // Two-level threshold on raw magnitudes: absolute noise floor and
    // relative-to-max floor. A bin must exceed both to be a candidate.
    float magSum = 0.0f;
    for (int i = 0; i < specSize; i++) magSum += mag[i];
    float noiseFloor = (magSum / (float)specSize) * 0.1f;
    float relFloor   = maxMag * HPCP_RELATIVE_THRESHOLD;
    float rawThreshold = noiseFloor > relFloor ? noiseFloor : relFloor;

    // Precompute: can we use IF estimation this frame?
    BOOL useIF = (ctx != NULL && ctx->hasPrev &&
                  ctx->specSize == specSize && ctx->hopSize > 0);

    float phaseAdvancePerBin = 0.0f;
    if (useIF) {
        phaseAdvancePerBin = 2.0f * (float)M_PI * (float)ctx->hopSize / (float)N;
    }

    // Precompute harmonic weights: harmonicWeight[h] = λ^h (0-indexed)
    float harmonicWeight[HPCP_MAX_HARMONIC];
    harmonicWeight[0] = 1.0f;
    for (int h = 1; h < HPCP_MAX_HARMONIC; h++) {
        harmonicWeight[h] = harmonicWeight[h - 1] * HPCP_HARMONIC_DECAY;
    }

    // ---- Peak detection and mapping ----
    //
    // A bin is accepted as a spectral peak if ALL of these hold:
    //   1. Raw magnitude > rawThreshold           (noise gate)
    //   2. Whitened local maximum                  (genuine peak shape)
    //   3. Whitened magnitude > WHITEN_PEAK_THRESHOLD  (stands out from local floor)
    //
    // Gate 1 eliminates silence and broadband noise.
    // Gates 2-3 eliminate sidelobe artifacts that pass gate 1 by riding
    // on a strong neighbor's skirt. After whitening, the neighbor's skirt
    // is flat (envelope-normalized), so only bins with genuine spectral
    // content survive.
    //
    // Once accepted, raw magnitude is used for energy calculation.

    int minBin = (int)(HPCP_MIN_FUNDAMENTAL_FREQ / freqRes);
    int maxBin = (int)(HPCP_MAX_PEAK_FREQ / freqRes);
    if (minBin < 1) minBin = 1;
    if (maxBin >= specSize - 1) maxBin = specSize - 2;

    int peakCount = 0;

    for (int bin = minBin; bin <= maxBin; bin++) {
        // Gate 1: raw magnitude noise floor
        if (mag[bin] <= rawThreshold) continue;

        // Gate 2: local maximum in whitened spectrum
        if (whitened[bin] <= whitened[bin - 1] || whitened[bin] <= whitened[bin + 1])
            continue;

        // Gate 3: whitened magnitude must stand out from local average
        if (whitened[bin] < HPCP_WHITEN_PEAK_THRESHOLD) continue;

        peakCount++;

        // ---- Frequency estimation ----
        // IF estimation uses raw FFT phase (whitening doesn't affect phase).
        // Energy uses raw magnitude (whitening is only for peak gating).

        float freq;
        float peakMag = mag[bin];

        if (useIF) {
            float curReal = fftData[2*bin];
            float curImag = fftData[2*bin+1];
            float curPhase = atan2f(curImag, curReal);

            float prevPhase = atan2f(ctx->prevImag[bin], ctx->prevReal[bin]);

            float actualAdvance = curPhase - prevPhase;
            float expectedAdvance = (float)bin * phaseAdvancePerBin;
            float deviation = PrincipalArg(actualAdvance - expectedAdvance);
            float freqOffset = deviation * sr / (2.0f * (float)M_PI * (float)ctx->hopSize);

            freq = (float)bin * freqRes + freqOffset;

            // Sanity check: IF within ±0.6 bins of peak center
            float binFreq = (float)bin * freqRes;
            if (freq < binFreq - 0.6f * freqRes || freq > binFreq + 0.6f * freqRes) {
                // IF unreliable — fall back to parabolic interpolation
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
            // Parabolic interpolation fallback (first frame)
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
        if (freq < HPCP_MIN_FUNDAMENTAL_FREQ || freq > HPCP_MAX_PEAK_FREQ)
            continue;

        // Raw energy for this peak
        float energy = peakMag * peakMag;

        // ---- Tuning offset accumulation ----
        // Record each peak's fractional-semitone offset from the nearest
        // integer semitone (relative to the default 440 Hz reference).
        // This data is used by EstimateTuning() to find the global tuning
        // offset. We only accumulate before tuning is locked; afterward,
        // the corrected refFreq already accounts for the offset.
        if (ctx != NULL && !ctx->tuningLocked) {
            float semi440 = 12.0f * log2f(freq / HPCP_REF_FREQ);
            float fractional = semi440 - roundf(semi440);  // -0.5 to +0.5 semitones
            int centOffset = (int)roundf(fractional * 100.0f);  // convert to cents
            // Clamp to histogram range
            if (centOffset >= -50 && centOffset <= 50) {
                int histBin = centOffset + 50;  // map [-50,+50] to [0,100]
                ctx->tuningHistogram[histBin] += energy;
                ctx->tuningPeakCount++;
            }
        }

        // ---- Harmonic folding with cosine-weighted bin assignment ----
        //
        // Each spectral peak at frequency f is attributed to candidate
        // fundamentals f/1, f/2, ..., f/H with exponentially decaying
        // weights. Each candidate's energy is then distributed between
        // the two nearest pitch classes using a raised cosine kernel
        // instead of hard rounding.
        //
        // The cosine kernel ensures that a peak exactly between two
        // pitch classes splits its energy 50/50, while a peak exactly
        // on a pitch class contributes 100% to that class. This
        // eliminates the quantization cliff where a 1-cent shift could
        // move 100% of energy from one bin to another.
        for (int h = 0; h < HPCP_MAX_HARMONIC; h++) {
            float candidateFreq = freq / (float)(h + 1);

            if (candidateFreq < HPCP_MIN_FUNDAMENTAL_FREQ)
                break;

            if (candidateFreq > HPCP_MAX_FUNDAMENTAL_FREQ)
                continue;

            float semi = 12.0f * log2f(candidateFreq / refFreq);
            float contribution = harmonicWeight[h] * energy;

            // Cosine-weighted distribution between two nearest pitch classes.
            // semi is a continuous semitone value relative to A4.
            // We decompose it into an integer part (lower pitch class)
            // and a fractional part d in [0, 1).
            //
            // Kernel: wUpper = 0.5 * (1 - cos(pi * d))
            //         wLower = 1 - wUpper
            //
            // At d=0:   wLower=1.0, wUpper=0.0  (exactly on lower bin)
            // At d=0.5: wLower=0.5, wUpper=0.5  (halfway between)
            // At d=1:   wLower=0.0, wUpper=1.0  (exactly on upper bin)
            float semiShifted = semi + 9.0f;   // shift so C=0 (A is +9)
            float semiMod = fmodf(semiShifted, 12.0f);
            if (semiMod < 0.0f) semiMod += 12.0f;

            int pcLow = (int)floorf(semiMod) % 12;
            int pcHigh = (pcLow + 1) % 12;
            float d = semiMod - floorf(semiMod);  // fractional part [0, 1)

            float wHigh = 0.5f * (1.0f - cosf((float)M_PI * d));
            float wLow = 1.0f - wHigh;

            result.bins[pcLow]  += wLow * contribution;
            result.bins[pcHigh] += wHigh * contribution;
            result.totalEnergy  += contribution;
        }
    }

    result.peakCount = peakCount;

    // Store current FFT for next frame's IF calculation
    if (ctx != NULL && ctx->specSize == specSize) {
        for (int i = 0; i < specSize; i++) {
            ctx->prevReal[i] = fftData[2*i];
            ctx->prevImag[i] = fftData[2*i+1];
        }
        ctx->hasPrev = TRUE;
    }

    free(whitened);
    free(envelope);
    free(mag);
    free(fftData);
    return result;
}
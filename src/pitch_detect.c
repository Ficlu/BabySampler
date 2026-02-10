// pitch_detect.c
//
// Pitch detection using the YIN algorithm.
// Based on: de Cheveigné & Kawahara, "YIN, a fundamental frequency estimator
// for speech and music", JASA 2002.
//
// Changes from original autocorrelation approach:
//   1. Difference function (measures dissimilarity, not similarity)
//   2. Cumulative mean normalization (eliminates octave/sub-harmonic errors)
//   3. Absolute threshold with first-dip selection (prefers fundamental)
//   4. Parabolic interpolation for sub-sample accuracy

#include "pitch_detect.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Note names for display
static const char* NOTE_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

void PitchConfig_InitVoice(PitchConfig *config, DWORD sampleRate)
{
    config->sampleRate = sampleRate;
    config->minFreq = 80.0f;
    config->maxFreq = 1200.0f;
    config->confidenceThreshold = 0.35f;
    config->harmonicityThreshold = 0.06f;
    config->transientThreshold = 4.0f;
    config->stabilityFrames = 2;
}

void PitchConfig_InitInstrument(PitchConfig *config, DWORD sampleRate)
{
    config->sampleRate = sampleRate;
    config->minFreq = 80.0f;
    config->maxFreq = 2000.0f;
    config->confidenceThreshold = 0.3f;
    config->harmonicityThreshold = 0.06f;
    config->transientThreshold = 6.0f;
    config->stabilityFrames = 1;
}

void PitchTracker_Init(PitchTracker *tracker)
{
    tracker->lastPitchClass = -1;
    tracker->stabilityCount = 0;
    tracker->lastAmplitude = 0.0f;
    tracker->initialized = FALSE;
}

int FreqToMidi(float freq)
{
    if (freq <= 0.0f) return -1;
    float midi = 69.0f + 12.0f * log2f(freq / 440.0f);
    return (int)roundf(midi);
}

int MidiToPitchClass(int midiNote)
{
    if (midiNote < 0) return -1;
    return ((midiNote % 12) + 12) % 12;
}

int FreqToPitchClass(float freq)
{
    return MidiToPitchClass(FreqToMidi(freq));
}

const char* PitchClassToName(int pitchClass)
{
    if (pitchClass < 0 || pitchClass > 11) return "?";
    return NOTE_NAMES[pitchClass];
}

void MidiToNoteName(int midiNote, char *buffer, int bufferSize)
{
    if (midiNote < 0) {
        snprintf(buffer, bufferSize, "---");
        return;
    }
    int pitchClass = MidiToPitchClass(midiNote);
    int octave = (midiNote / 12) - 1;
    snprintf(buffer, bufferSize, "%s%d", NOTE_NAMES[pitchClass], octave);
}

float CalculateRMS(const float *samples, int sampleCount)
{
    if (!samples || sampleCount <= 0) return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < sampleCount; i++) {
        sum += samples[i] * samples[i];
    }
    return sqrtf(sum / sampleCount);
}

// Check whether an analysis window is temporally stable (no note transitions).
// Compares RMS energy in the first and second halves of the buffer. If they
// differ by more than maxDbDiff dB, the frame likely straddles a note onset
// or offset, and pitch detection on it will produce phantom frequencies from
// the mix of two notes.
//
// Returns TRUE if the frame is stable (safe to analyze).
BOOL IsFrameStable(const float *samples, int sampleCount, float maxDbDiff)
{
    if (!samples || sampleCount < 4) return TRUE;

    int halfCount = sampleCount / 2;
    float rmsFirst  = CalculateRMS(samples, halfCount);
    float rmsSecond = CalculateRMS(samples + halfCount, halfCount);

    // Both halves silent — no transition, just silence
    if (rmsFirst < 0.001f && rmsSecond < 0.001f) return TRUE;

    // One half silent, other not — definitely a transition
    if (rmsFirst < 0.001f || rmsSecond < 0.001f) return FALSE;

    float ratio = rmsFirst > rmsSecond
                  ? rmsFirst / rmsSecond
                  : rmsSecond / rmsFirst;

    float dbDiff = 20.0f * log10f(ratio);
    return dbDiff <= maxDbDiff;
}

// Check for harmonic structure at the detected frequency.
// Returns 0-1 indicating how harmonic the signal is.
static float CheckHarmonicity(const float *samples, int sampleCount,
                               DWORD sampleRate, float fundamentalFreq)
{
    if (fundamentalFreq <= 0.0f) return 0.0f;

    int fundamentalLag = (int)(sampleRate / fundamentalFreq);
    if (fundamentalLag < 2 || fundamentalLag * 3 >= sampleCount) return 0.5f;

    float corrFundamental = 0.0f;
    float corrHarmonic2 = 0.0f;
    float energy = 0.0f;

    int windowSize = sampleCount - fundamentalLag * 2;
    if (windowSize < fundamentalLag) windowSize = fundamentalLag;

    for (int i = 0; i < windowSize; i++) {
        energy += samples[i] * samples[i];
        corrFundamental += samples[i] * samples[i + fundamentalLag];
        if (i + fundamentalLag * 2 < sampleCount) {
            corrHarmonic2 += samples[i] * samples[i + fundamentalLag * 2];
        }
    }

    if (energy < 0.0001f) return 0.0f;

    corrFundamental /= energy;
    corrHarmonic2 /= energy;

    float harmonicity = (corrFundamental + corrHarmonic2 * 0.5f) / 1.5f;
    if (harmonicity < 0.0f) harmonicity = 0.0f;
    if (harmonicity > 1.0f) harmonicity = 1.0f;

    return harmonicity;
}

// YIN pitch detection algorithm
//
// The key insight of YIN vs raw autocorrelation:
//
// Autocorrelation finds where the signal is MOST SIMILAR to a shifted copy.
// Problem: it naturally has peaks at the fundamental AND at all sub-harmonics,
// so it can lock onto 2x or 0.5x the true pitch (octave errors).
//
// YIN instead measures DIFFERENCE (how UN-similar the signal is at each lag),
// then normalizes cumulatively. The cumulative mean normalization ensures that
// the first dip (the fundamental period) is always the deepest relative dip,
// even if the absolute difference at a sub-harmonic lag happens to be lower.
PitchResult DetectPitch(const float *samples, int sampleCount, const PitchConfig *config)
{
    PitchResult result = {0.0f, 0.0f, 0.0f, -1, -1};

    if (!samples || sampleCount < 2 || !config) {
        return result;
    }

    float sampleRate = (float)config->sampleRate;

    // Calculate lag range from frequency range
    int minLag = (int)(sampleRate / config->maxFreq);
    int maxLag = (int)(sampleRate / config->minFreq);

    // YIN uses half the buffer as the integration window (W = N/2)
    int W = sampleCount / 2;

    // Clamp to valid range
    if (minLag < 1) minLag = 1;
    if (maxLag >= W) maxLag = W - 1;
    if (minLag >= maxLag) {
        return result;
    }

    // Check signal energy — skip silent frames
    float rms = CalculateRMS(samples, sampleCount);
    if (rms < 0.005f) {
        return result;
    }

    // --- YIN Step 1: Difference function ---
    // d(τ) = Σ_{j=0}^{W-1} (x[j] - x[j+τ])²
    //
    // Unlike autocorrelation (which computes Σ x[j]*x[j+τ]), this measures
    // how DIFFERENT the signal is from its shifted copy. The minimum of d(τ)
    // corresponds to the period of the signal.
    float *diffFunc = (float *)malloc((maxLag + 1) * sizeof(float));
    if (!diffFunc) return result;

    diffFunc[0] = 0.0f;

    for (int tau = 1; tau <= maxLag; tau++) {
        float sum = 0.0f;
        for (int j = 0; j < W; j++) {
            float delta = samples[j] - samples[j + tau];
            sum += delta * delta;
        }
        diffFunc[tau] = sum;
    }

    // --- YIN Step 2: Cumulative mean normalized difference function ---
    // d'(τ) = d(τ) / ((1/τ) * Σ_{j=1}^{τ} d(j))
    //
    // This normalization is what eliminates octave errors. Raw d(τ) tends to
    // decrease with increasing τ (because there are fewer samples contributing),
    // which biases toward longer lags (lower frequencies). The cumulative mean
    // normalization removes this bias and makes the threshold absolute.
    //
    // The function starts at 1.0 (by definition) and dips below 1.0 at periodic
    // lags. The FIRST significant dip corresponds to the fundamental period.
    float *cmndf = (float *)malloc((maxLag + 1) * sizeof(float));
    if (!cmndf) {
        free(diffFunc);
        return result;
    }

    cmndf[0] = 1.0f;
    float runningSum = 0.0f;

    for (int tau = 1; tau <= maxLag; tau++) {
        runningSum += diffFunc[tau];
        if (runningSum > 0.0f) {
            cmndf[tau] = diffFunc[tau] * (float)tau / runningSum;
        } else {
            cmndf[tau] = 1.0f;
        }
    }

    // --- YIN Step 3: Absolute threshold with first-dip selection ---
    // Instead of finding the global minimum (which raw autocorrelation does),
    // YIN picks the FIRST dip below a threshold. This is critical because:
    //   - The first dip = fundamental period
    //   - Later dips = sub-harmonics (octave below, etc.)
    // By taking the first qualifying dip, we always get the fundamental.
    //
    // Threshold 0.20: the YIN paper suggests 0.10-0.20. Lower values are
    // stricter (fewer detections, fewer errors). 0.20 is appropriate for
    // real-world audio with reverb and texture; 0.10 would be better for
    // clean monophonic signals like a tuner.
    float yinThreshold = 0.20f;
    int bestLag = 0;
    float bestVal = 1.0f;

    for (int tau = minLag; tau <= maxLag; tau++) {
        if (cmndf[tau] < yinThreshold) {
            // Found a dip below threshold — walk to the local minimum
            while (tau + 1 <= maxLag && cmndf[tau + 1] < cmndf[tau]) {
                tau++;
            }
            bestLag = tau;
            bestVal = cmndf[tau];
            break;  // Take the FIRST qualifying dip
        }
    }

    // Fallback: if no dip below threshold, use global minimum (reduced confidence)
    if (bestLag == 0) {
        for (int tau = minLag; tau <= maxLag; tau++) {
            if (cmndf[tau] < bestVal) {
                bestVal = cmndf[tau];
                bestLag = tau;
            }
        }
        if (bestVal > 0.5f || bestLag == 0) {
            free(diffFunc);
            free(cmndf);
            return result;
        }
    }

    // --- YIN Step 4: Parabolic interpolation ---
    // Refine the lag estimate to sub-sample accuracy by fitting a parabola
    // through the minimum and its two neighbors.
    float betterLag = (float)bestLag;
    if (bestLag > minLag && bestLag < maxLag) {
        float s0 = cmndf[bestLag - 1];
        float s1 = cmndf[bestLag];
        float s2 = cmndf[bestLag + 1];

        float denom = 2.0f * (2.0f * s1 - s0 - s2);
        if (fabsf(denom) > 0.0001f) {
            float delta = (s0 - s2) / denom;
            if (fabsf(delta) < 1.0f) {
                betterLag = (float)bestLag + delta;
            }
        }
    }

    // Convert lag to frequency
    float freq = sampleRate / betterLag;

    // Convert YIN's cmndf value to a confidence score (0-1, higher = better)
    // cmndf: 0.0 = perfect periodicity, 1.0 = no periodicity
    float confidence = 1.0f - bestVal;
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;

    // Apply the config's confidence threshold
    if (confidence < config->confidenceThreshold) {
        free(diffFunc);
        free(cmndf);
        return result;
    }

    // Secondary validation: check harmonic structure
    float harmonicity = CheckHarmonicity(samples, sampleCount, config->sampleRate, freq);

    if (harmonicity < config->harmonicityThreshold) {
        free(diffFunc);
        free(cmndf);
        return result;
    }

    result.frequency = freq;
    result.confidence = confidence;
    result.harmonicity = harmonicity;
    result.midiNote = FreqToMidi(result.frequency);
    result.pitchClass = MidiToPitchClass(result.midiNote);

    free(diffFunc);
    free(cmndf);

    return result;
}

// Detect pitch with tracking for stability and transient rejection.
// If outConfidence is non-NULL, writes the YIN confidence score for the
// detected pitch (useful for weighting the scale histogram).
int DetectPitchTracked(const float *samples, int sampleCount,
                       const PitchConfig *config, PitchTracker *tracker,
                       float *outConfidence)
{
    if (!samples || !config || !tracker) return -1;

    if (outConfidence) *outConfidence = 0.0f;

    // Calculate current amplitude
    float currentAmplitude = CalculateRMS(samples, sampleCount);

    // Transient detection: reject sudden amplitude spikes
    if (tracker->initialized && tracker->lastAmplitude > 0.001f) {
        float ampRatio = currentAmplitude / tracker->lastAmplitude;
        if (ampRatio > config->transientThreshold) {
            tracker->lastAmplitude = currentAmplitude;
            tracker->stabilityCount = 0;
            return -1;
        }
    }

    tracker->lastAmplitude = currentAmplitude;
    tracker->initialized = TRUE;

    // Detect pitch using YIN
    PitchResult pitch = DetectPitch(samples, sampleCount, config);

    if (pitch.pitchClass < 0) {
        tracker->stabilityCount = 0;
        return -1;
    }

    // Check pitch stability
    if (pitch.pitchClass == tracker->lastPitchClass) {
        tracker->stabilityCount++;
    } else {
        tracker->lastPitchClass = pitch.pitchClass;
        tracker->stabilityCount = 1;
    }

    // Only return pitch if it's been stable long enough
    if (tracker->stabilityCount >= config->stabilityFrames) {
        if (outConfidence) *outConfidence = pitch.confidence;
        return pitch.pitchClass;
    }

    return -1;
}
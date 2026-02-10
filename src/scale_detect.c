// scale_detect.c
//
// Key detection using Krumhansl-Kessler pitch class profiles.
//
// Previous approach: binary scale templates (1/0 for each scale degree),
// scored by what percentage of detected notes fell on scale degrees.
// Problem: every scale degree weighted equally, and with sparse data
// almost any set of notes would perfectly match some obscure scale.
//
// New approach: Krumhansl-Kessler (1990) key profiles are empirically
// derived weightings that reflect how often each pitch class appears
// in real music in a given key. The tonic has the highest weight,
// followed by the fifth, then the third, etc. We use Pearson correlation
// between the observed pitch distribution and each of the 24 candidate
// key profiles (12 roots × major/minor). This naturally handles:
//   - Sparse data (correlation is scale-invariant)
//   - Out-of-scale notes (they just reduce correlation slightly)
//   - Tonic/dominant emphasis (built into the profiles)

#include "scale_detect.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

// Note names
static const char* NOTE_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Krumhansl-Kessler major key profile (Krumhansl, 1990)
// Index 0 = tonic, index 7 = dominant (fifth)
// These values represent the perceived stability/importance of each
// scale degree in a major key context.
static const float KK_MAJOR[12] = {
    6.35f,  // I   (tonic)
    2.23f,  // #I
    3.48f,  // II
    2.33f,  // #II
    4.38f,  // III
    4.09f,  // IV
    2.52f,  // #IV
    5.19f,  // V   (dominant)
    2.39f,  // #V
    3.66f,  // VI
    2.29f,  // #VI
    2.88f   // VII
};

// Krumhansl-Kessler minor key profile (Krumhansl, 1990)
// Reflects natural minor with some harmonic minor influence
// (raised 7th has moderate weight).
static const float KK_MINOR[12] = {
    6.33f,  // I   (tonic)
    2.68f,  // #I
    3.52f,  // II
    5.38f,  // III (minor third - very important)
    2.60f,  // #III
    3.53f,  // IV
    2.54f,  // #IV
    4.75f,  // V   (dominant)
    3.98f,  // #V  (minor sixth)
    2.69f,  // VI
    3.34f,  // #VI (minor seventh)
    3.17f   // VII (leading tone - harmonic minor influence)
};

void ScaleAccumulator_Init(ScaleAccumulator *acc, DWORD sampleRate)
{
    memset(acc->histogram, 0, sizeof(acc->histogram));
    acc->totalCount = 0;
    acc->totalWeight = 0.0f;
    acc->sampleRate = sampleRate;
}

void ScaleAccumulator_Reset(ScaleAccumulator *acc)
{
    memset(acc->histogram, 0, sizeof(acc->histogram));
    acc->totalCount = 0;
    acc->totalWeight = 0.0f;
}

void ScaleAccumulator_AddPitch(ScaleAccumulator *acc, int pitchClass, float weight)
{
    if (pitchClass >= 0 && pitchClass < 12 && weight > 0.0f) {
        acc->histogram[pitchClass] += weight;
        acc->totalCount++;
        acc->totalWeight += weight;
    }
}

void ScaleAccumulator_AddChromagram(ScaleAccumulator *acc, const float chromagram[12])
{
    // Check if the frame has any meaningful energy
    float frameEnergy = 0.0f;
    for (int i = 0; i < 12; i++) {
        frameEnergy += chromagram[i];
    }
    if (frameEnergy < 0.0001f) return;

    // Add all 12 bins directly to the histogram.
    // Unlike AddPitch (which adds one pitch class per call), this
    // adds the entire spectral energy distribution at once, capturing
    // all simultaneous pitch classes in a polyphonic signal.
    for (int i = 0; i < 12; i++) {
        acc->histogram[i] += chromagram[i];
    }
    acc->totalWeight += frameEnergy;
    acc->totalCount++;
}

// Compute Pearson correlation between two 12-element arrays.
// Returns value in [-1, 1]. Higher = better match.
//
// Pearson correlation measures the linear relationship between two
// distributions, independent of their scale or offset. This is exactly
// what we want: a recording with few detections should match just as
// well as one with many, as long as the shape of the distribution
// matches the key profile.
static float PearsonCorrelation(const float *x, const float *y, int n)
{
    float sumX = 0.0f, sumY = 0.0f;
    float sumXX = 0.0f, sumYY = 0.0f, sumXY = 0.0f;

    for (int i = 0; i < n; i++) {
        sumX += x[i];
        sumY += y[i];
    }

    float meanX = sumX / (float)n;
    float meanY = sumY / (float)n;

    for (int i = 0; i < n; i++) {
        float dx = x[i] - meanX;
        float dy = y[i] - meanY;
        sumXX += dx * dx;
        sumYY += dy * dy;
        sumXY += dx * dy;
    }

    float denom = sqrtf(sumXX * sumYY);
    if (denom < 0.0001f) return 0.0f;

    return sumXY / denom;
}

// Rotate a 12-element array by 'shift' positions.
// This aligns the K-K profile so that index 0 corresponds to the
// candidate root note.
static void RotateProfile(const float *src, float *dst, int shift)
{
    for (int i = 0; i < 12; i++) {
        dst[i] = src[(i - shift + 12) % 12];
    }
}

ScaleResult ScaleAccumulator_Analyze(const ScaleAccumulator *acc)
{
    ScaleResult result = {-1, SCALE_UNKNOWN, 0.0f, acc->totalCount};

    // Need minimum data to make a meaningful analysis.
    // With confidence weighting, even a few strong detections can be useful,
    // but we still need at least 4 distinct pitch events.
    if (acc->totalCount < 4) {
        return result;
    }

    // Count how many distinct pitch classes were observed
    int distinctPitches = 0;
    for (int i = 0; i < 12; i++) {
        if (acc->histogram[i] > 0.0f) distinctPitches++;
    }

    // Need at least 3 distinct pitch classes to distinguish keys.
    // With fewer, too many keys would match equally well.
    if (distinctPitches < 3) {
        return result;
    }

    float bestCorrelation = -2.0f;
    int bestRoot = 0;
    ScaleType bestType = SCALE_UNKNOWN;

    // Test all 24 candidate keys: 12 roots × {major, minor}
    for (int root = 0; root < 12; root++) {
        float rotatedProfile[12];

        // Test major key with this root
        RotateProfile(KK_MAJOR, rotatedProfile, root);
        float corrMajor = PearsonCorrelation(acc->histogram, rotatedProfile, 12);

        if (corrMajor > bestCorrelation) {
            bestCorrelation = corrMajor;
            bestRoot = root;
            bestType = SCALE_MAJOR;
        }

        // Test minor key with this root
        RotateProfile(KK_MINOR, rotatedProfile, root);
        float corrMinor = PearsonCorrelation(acc->histogram, rotatedProfile, 12);

        if (corrMinor > bestCorrelation) {
            bestCorrelation = corrMinor;
            bestRoot = root;
            bestType = SCALE_NATURAL_MINOR;
        }
    }

    // Convert Pearson correlation to a 0-1 confidence score.
    // Pearson r ranges from -1 to 1. In practice, good key matches
    // produce r > 0.5, and strong matches r > 0.7.
    // We map: r <= 0 → 0, r = 1 → 1
    float confidence = bestCorrelation;
    if (confidence < 0.0f) confidence = 0.0f;

    // Require minimum correlation for a valid result.
    // Below 0.3, the match is essentially random.
    if (confidence < 0.3f) {
        return result;
    }

    result.rootNote = bestRoot;
    result.scaleType = bestType;
    result.confidence = confidence;

    return result;
}

const char* ScaleType_GetName(ScaleType type)
{
    switch (type) {
        case SCALE_MAJOR:           return "Major";
        case SCALE_NATURAL_MINOR:   return "Minor";
        case SCALE_DORIAN:          return "Dorian";
        case SCALE_PHRYGIAN:        return "Phrygian";
        case SCALE_LYDIAN:          return "Lydian";
        case SCALE_MIXOLYDIAN:      return "Mixolydian";
        case SCALE_LOCRIAN:         return "Locrian";
        case SCALE_HARMONIC_MINOR:  return "Harm Min";
        case SCALE_MELODIC_MINOR:   return "Mel Min";
        case SCALE_PENTATONIC_MAJOR:return "Pent Maj";
        case SCALE_PENTATONIC_MINOR:return "Pent Min";
        case SCALE_BLUES:           return "Blues";
        default:                    return "Unknown";
    }
}

static const char* ScaleType_GetShortName(ScaleType type)
{
    switch (type) {
        case SCALE_MAJOR:           return "maj";
        case SCALE_NATURAL_MINOR:   return "m";
        case SCALE_DORIAN:          return "dor";
        case SCALE_PHRYGIAN:        return "phr";
        case SCALE_LYDIAN:          return "lyd";
        case SCALE_MIXOLYDIAN:      return "mix";
        case SCALE_LOCRIAN:         return "loc";
        case SCALE_HARMONIC_MINOR:  return "hm";
        case SCALE_MELODIC_MINOR:   return "mm";
        case SCALE_PENTATONIC_MAJOR:return "pM";
        case SCALE_PENTATONIC_MINOR:return "pm";
        case SCALE_BLUES:           return "blu";
        default:                    return "?";
    }
}

void ScaleResult_GetName(const ScaleResult *result, char *buffer, int bufferSize)
{
    if (result->scaleType == SCALE_UNKNOWN || result->rootNote < 0) {
        snprintf(buffer, bufferSize, "Unknown");
        return;
    }

    snprintf(buffer, bufferSize, "%s %s",
             NOTE_NAMES[result->rootNote],
             ScaleType_GetName(result->scaleType));
}

void ScaleResult_GetShortName(const ScaleResult *result, char *buffer, int bufferSize)
{
    if (result->scaleType == SCALE_UNKNOWN || result->rootNote < 0) {
        snprintf(buffer, bufferSize, "?");
        return;
    }

    const char *shortScale = ScaleType_GetShortName(result->scaleType);

    // For major, just show the note name (e.g., "C" instead of "Cmaj")
    if (result->scaleType == SCALE_MAJOR) {
        snprintf(buffer, bufferSize, "%s", NOTE_NAMES[result->rootNote]);
    } else {
        snprintf(buffer, bufferSize, "%s%s", NOTE_NAMES[result->rootNote], shortScale);
    }
}
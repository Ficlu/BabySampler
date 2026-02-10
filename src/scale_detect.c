// scale_detect.c
//
// Key detection using Krumhansl-Kessler pitch class profiles.
//
// Uses Pearson correlation between the observed pitch distribution and
// each of the 24 candidate key profiles (12 roots × major/minor).
// See Krumhansl, "Cognitive Foundations of Musical Pitch" (1990).

#include "scale_detect.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

// Note names
static const char* NOTE_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Krumhansl-Kessler major key profile (Krumhansl, 1990)
static const float KK_MAJOR[12] = {
    6.35f,  2.23f,  3.48f,  2.33f,  4.38f,  4.09f,
    2.52f,  5.19f,  2.39f,  3.66f,  2.29f,  2.88f
};

// Krumhansl-Kessler minor key profile (Krumhansl, 1990)
static const float KK_MINOR[12] = {
    6.33f,  2.68f,  3.52f,  5.38f,  2.60f,  3.53f,
    2.54f,  4.75f,  3.98f,  2.69f,  3.34f,  3.17f
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
    float frameEnergy = 0.0f;
    for (int i = 0; i < 12; i++) {
        frameEnergy += chromagram[i];
    }
    if (frameEnergy < 0.0001f) return;

    float invEnergy = 1.0f / frameEnergy;
    for (int i = 0; i < 12; i++) {
        acc->histogram[i] += chromagram[i] * invEnergy;
    }
    acc->totalWeight += 1.0f;
    acc->totalCount++;
}

// Pearson correlation between two 12-element arrays.
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
static void RotateProfile(const float *src, float *dst, int shift)
{
    for (int i = 0; i < 12; i++) {
        dst[i] = src[(i - shift + 12) % 12];
    }
}

ScaleResult ScaleAccumulator_Analyze(const ScaleAccumulator *acc)
{
    ScaleResult result = {-1, SCALE_UNKNOWN, 0.0f, acc->totalCount};

    if (acc->totalCount < 4) {
        return result;
    }

    int distinctPitches = 0;
    for (int i = 0; i < 12; i++) {
        if (acc->histogram[i] > 0.0f) distinctPitches++;
    }

    if (distinctPitches < 3) {
        return result;
    }

    float bestCorrelation = -2.0f;
    int bestRoot = 0;
    ScaleType bestType = SCALE_UNKNOWN;

    for (int root = 0; root < 12; root++) {
        float rotatedProfile[12];

        RotateProfile(KK_MAJOR, rotatedProfile, root);
        float corrMajor = PearsonCorrelation(acc->histogram, rotatedProfile, 12);

        if (corrMajor > bestCorrelation) {
            bestCorrelation = corrMajor;
            bestRoot = root;
            bestType = SCALE_MAJOR;
        }

        RotateProfile(KK_MINOR, rotatedProfile, root);
        float corrMinor = PearsonCorrelation(acc->histogram, rotatedProfile, 12);

        if (corrMinor > bestCorrelation) {
            bestCorrelation = corrMinor;
            bestRoot = root;
            bestType = SCALE_NATURAL_MINOR;
        }
    }

    float confidence = bestCorrelation;
    if (confidence < 0.0f) confidence = 0.0f;

    if (confidence < 0.3f) {
        return result;
    }

    result.rootNote = bestRoot;
    result.scaleType = bestType;
    result.confidence = confidence;

    return result;
}

// ---- Diagnostics ----

// Candidate entry for sorting
typedef struct {
    int root;
    ScaleType type;
    float correlation;
} KeyCandidate;

// Compare for descending sort
static int CmpCandidateDesc(const void *a, const void *b)
{
    float ca = ((const KeyCandidate *)a)->correlation;
    float cb = ((const KeyCandidate *)b)->correlation;
    if (cb > ca) return 1;
    if (cb < ca) return -1;
    return 0;
}

void ScaleAccumulator_PrintDiagnostics(const ScaleAccumulator *acc,
                                        const ScaleResult *result,
                                        int totalPeaks, int totalFrames)
{
    // ---- Header ----
    printf("\n=== Scale Analysis ===\n");
    printf("Frames: %d accumulated (%d total), Peaks: %d (%.1f/frame)\n",
           acc->totalCount, totalFrames, totalPeaks,
           totalFrames > 0 ? (float)totalPeaks / (float)totalFrames : 0.0f);

    // ---- Histogram: all 12 bins, always shown ----
    float totalEnergy = 0.0f;
    for (int i = 0; i < 12; i++) {
        totalEnergy += acc->histogram[i];
    }

    printf("Histogram:\n");
    // Two rows of 6 for readability
    for (int row = 0; row < 2; row++) {
        printf("  ");
        for (int col = 0; col < 6; col++) {
            int i = row * 6 + col;
            float pct = totalEnergy > 0.0f
                        ? 100.0f * acc->histogram[i] / totalEnergy
                        : 0.0f;
            // Right-align note name in 2 chars, then percentage
            printf("%-2s %5.1f%%  ", NOTE_NAMES[i], pct);
        }
        printf("\n");
    }

    // ---- Top 5 key candidates ----
    // Compute all 24 correlations, sort, print top 5
    KeyCandidate candidates[24];
    int nCand = 0;

    for (int root = 0; root < 12; root++) {
        float rotated[12];

        RotateProfile(KK_MAJOR, rotated, root);
        candidates[nCand].root = root;
        candidates[nCand].type = SCALE_MAJOR;
        candidates[nCand].correlation = PearsonCorrelation(acc->histogram, rotated, 12);
        nCand++;

        RotateProfile(KK_MINOR, rotated, root);
        candidates[nCand].root = root;
        candidates[nCand].type = SCALE_NATURAL_MINOR;
        candidates[nCand].correlation = PearsonCorrelation(acc->histogram, rotated, 12);
        nCand++;
    }

    qsort(candidates, nCand, sizeof(KeyCandidate), CmpCandidateDesc);

    int showCount = 5;
    if (showCount > nCand) showCount = nCand;

    printf("Top candidates:\n");
    for (int i = 0; i < showCount; i++) {
        const char *typeName = (candidates[i].type == SCALE_MAJOR) ? "Major" : "Minor";
        printf("  %d. %-2s %-5s  r=%.3f%s\n",
               i + 1,
               NOTE_NAMES[candidates[i].root],
               typeName,
               candidates[i].correlation,
               i == 0 ? "  <-- best" : "");
    }

    // ---- In-scale vs out-of-scale energy ----
    // Use the detected key's scale degrees to compute the ratio
    if (result->scaleType != SCALE_UNKNOWN && result->rootNote >= 0) {
        // Major scale degrees: 0 2 4 5 7 9 11 (relative to root)
        // Minor scale degrees: 0 2 3 5 7 8 10 (relative to root)
        static const int majorDegrees[] = {0, 2, 4, 5, 7, 9, 11};
        static const int minorDegrees[] = {0, 2, 3, 5, 7, 8, 10};

        const int *degrees = (result->scaleType == SCALE_MAJOR) ? majorDegrees : minorDegrees;
        int nDegrees = 7;

        // Build a lookup of which pitch classes are in-scale
        int inScale[12] = {0};
        for (int d = 0; d < nDegrees; d++) {
            int pc = (result->rootNote + degrees[d]) % 12;
            inScale[pc] = 1;
        }

        float inEnergy = 0.0f, outEnergy = 0.0f;
        for (int i = 0; i < 12; i++) {
            if (inScale[i])
                inEnergy += acc->histogram[i];
            else
                outEnergy += acc->histogram[i];
        }

        float inPct = totalEnergy > 0.0f ? 100.0f * inEnergy / totalEnergy : 0.0f;
        float outPct = totalEnergy > 0.0f ? 100.0f * outEnergy / totalEnergy : 0.0f;

        char keyName[32];
        ScaleResult_GetName(result, keyName, sizeof(keyName));
        printf("Key: %s  In-scale: %.1f%%  Out-of-scale: %.1f%%\n", keyName, inPct, outPct);
    } else {
        printf("Key: Unknown (insufficient data or low correlation)\n");
    }

    printf("========================\n\n");
}

// ---- Name formatting (unchanged) ----

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

    if (result->scaleType == SCALE_MAJOR) {
        snprintf(buffer, bufferSize, "%s", NOTE_NAMES[result->rootNote]);
    } else {
        snprintf(buffer, bufferSize, "%s%s", NOTE_NAMES[result->rootNote], shortScale);
    }
}
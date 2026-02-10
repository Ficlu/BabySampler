// scale_detect.h
#ifndef SCALE_DETECT_H
#define SCALE_DETECT_H

#include <windows.h>

// Scale/key types.
// Primary detection uses Krumhansl-Kessler profiles for major/minor.
// Other modes retained for compatibility and future refinement.
typedef enum {
    SCALE_UNKNOWN = 0,
    SCALE_MAJOR,
    SCALE_NATURAL_MINOR,
    SCALE_DORIAN,
    SCALE_PHRYGIAN,
    SCALE_LYDIAN,
    SCALE_MIXOLYDIAN,
    SCALE_LOCRIAN,
    SCALE_HARMONIC_MINOR,
    SCALE_MELODIC_MINOR,
    SCALE_PENTATONIC_MAJOR,
    SCALE_PENTATONIC_MINOR,
    SCALE_BLUES,
} ScaleType;

// Result of scale analysis
typedef struct {
    int rootNote;       // 0-11 pitch class (C=0), -1 if unknown
    ScaleType scaleType;
    float confidence;   // Pearson correlation with K-K profile (0-1)
    int totalNotes;     // Total number of pitch detections
} ScaleResult;

// Accumulates pitch observations weighted by detection confidence.
// Uses float histogram so high-confidence detections contribute more
// to the scale analysis than uncertain ones.
typedef struct {
    float histogram[12]; // Confidence-weighted pitch class distribution
    int totalCount;      // Number of pitch detections (for minimum data checks)
    float totalWeight;   // Sum of all confidence weights
    DWORD sampleRate;
} ScaleAccumulator;

// Initialize / reset the accumulator
void ScaleAccumulator_Init(ScaleAccumulator *acc, DWORD sampleRate);
void ScaleAccumulator_Reset(ScaleAccumulator *acc);

// Add a pitch observation with its confidence weight.
// Higher-confidence detections contribute more to the distribution.
void ScaleAccumulator_AddPitch(ScaleAccumulator *acc, int pitchClass, float weight);

// Add a full 12-bin chromagram frame to the accumulator.
// Used with HPCP for polyphonic key detection. Each bin's energy
// is added directly to the histogram, so loud frames contribute
// more than quiet ones (which is desirable).
void ScaleAccumulator_AddChromagram(ScaleAccumulator *acc, const float chromagram[12]);

// Analyze the accumulated pitch distribution against Krumhansl-Kessler
// key profiles using Pearson correlation. Returns the best-matching key.
ScaleResult ScaleAccumulator_Analyze(const ScaleAccumulator *acc);

// Name formatting
const char* ScaleType_GetName(ScaleType type);
void ScaleResult_GetName(const ScaleResult *result, char *buffer, int bufferSize);
void ScaleResult_GetShortName(const ScaleResult *result, char *buffer, int bufferSize);

#endif
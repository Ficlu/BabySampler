// pitch_detect.h
#ifndef PITCH_DETECT_H
#define PITCH_DETECT_H

#include <windows.h>

// Configuration for pitch detection sensitivity
typedef struct {
    DWORD sampleRate;
    float minFreq;              // Lowest frequency to detect (Hz)
    float maxFreq;              // Highest frequency to detect (Hz)
    float confidenceThreshold;  // Minimum YIN confidence to accept (0-1)
    float harmonicityThreshold; // Minimum harmonicity score (0-1)
    float transientThreshold;   // Max amplitude ratio for transient rejection
    int stabilityFrames;        // Consecutive frames needed for stable detection
} PitchConfig;

// State for pitch tracking across consecutive frames
typedef struct {
    int lastPitchClass;
    int stabilityCount;
    float lastAmplitude;
    BOOL initialized;
} PitchTracker;

// Result of a single pitch detection
typedef struct {
    float frequency;    // Detected frequency in Hz (0 if none)
    float confidence;   // YIN confidence score (0-1, higher = more periodic)
    float harmonicity;  // Harmonicity check score (0-1)
    int midiNote;       // MIDI note number (-1 if none)
    int pitchClass;     // Pitch class 0-11 (-1 if none)
} PitchResult;

// Preset configurations
void PitchConfig_InitVoice(PitchConfig *config, DWORD sampleRate);
void PitchConfig_InitInstrument(PitchConfig *config, DWORD sampleRate);
void PitchTracker_Init(PitchTracker *tracker);

// Pitch/note conversion utilities
int FreqToMidi(float freq);
int MidiToPitchClass(int midiNote);
int FreqToPitchClass(float freq);
const char* PitchClassToName(int pitchClass);
void MidiToNoteName(int midiNote, char *buffer, int bufferSize);

// Signal analysis utilities
float CalculateRMS(const float *samples, int sampleCount);
float CalculateSpectralFlatness(const float *samples, int sampleCount, DWORD sampleRate);
BOOL IsFrameStable(const float *samples, int sampleCount, float maxDbDiff);

// Core pitch detection (YIN algorithm)
PitchResult DetectPitch(const float *samples, int sampleCount, const PitchConfig *config);

// Tracked pitch detection with stability and transient rejection.
// Returns pitch class (0-11) or -1 if no stable pitch detected.
// If outConfidence is non-NULL, writes the YIN confidence score (0-1).
int DetectPitchTracked(const float *samples, int sampleCount,
                       const PitchConfig *config, PitchTracker *tracker,
                       float *outConfidence);

#endif
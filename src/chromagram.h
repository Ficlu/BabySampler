// chromagram.h
//
// Harmonic Pitch Class Profile (HPCP) with Instantaneous Frequency
// refinement for sub-bin pitch accuracy at all octaves.
//
// Standard HPCP maps FFT peaks to pitch classes using parabolic
// interpolation, which has ±1-2 Hz error. At low frequencies
// (e.g., C2 = 65 Hz, semitone width = 3.7 Hz), this error is
// large enough to misattribute pitch classes.
//
// This implementation adds phase-derivative Instantaneous Frequency
// (IF) estimation: by comparing the phase of each FFT bin across
// consecutive frames, we recover the *exact* frequency of the
// energy in that bin (for isolated sinusoidal components). This
// gives sub-Hz accuracy regardless of FFT size, solving the
// low-octave resolution problem without affecting high octaves.
//
// Theory: Müller, "Fundamentals of Music Processing" (2015), §8.2.1
// Method: Kodera, Gendrin & de Villedary (1976); Auger & Flandrin (1995)

#ifndef CHROMAGRAM_H
#define CHROMAGRAM_H

#include <windows.h>

#define CHROMA_BINS 12

// Frequency range for pitch detection.
// Peaks outside this range are ignored.
#define HPCP_MIN_FUNDAMENTAL_FREQ  55.0f   // Just below A1
#define HPCP_MAX_FUNDAMENTAL_FREQ  2000.0f // Covers most musical content

// Reference frequency for pitch class mapping (A4)
#define HPCP_REF_FREQ 440.0f

// Result of HPCP analysis for one frame
typedef struct {
    float bins[CHROMA_BINS];  // Energy per pitch class (C=0, C#=1, ... B=11)
    float totalEnergy;        // Sum of all bin energies
} Chromagram;

// Persistent context for phase-derivative IF estimation.
// Stores the complex FFT output from the previous frame so we can
// compute instantaneous frequency from inter-frame phase differences.
typedef struct {
    float *prevReal;     // Previous frame FFT real parts [specSize]
    float *prevImag;     // Previous frame FFT imaginary parts [specSize]
    int    specSize;     // N/2 (number of frequency bins)
    int    hopSize;      // Hop size in samples (for IF calculation)
    BOOL   hasPrev;      // FALSE until first frame has been processed
} ChromagramContext;

// Initialize/free the IF context.
// fftSize must be a power of 2 (e.g., 8192).
// hopSize is the hop between consecutive analysis frames.
BOOL ChromagramContext_Init(ChromagramContext *ctx, int fftSize, int hopSize);
void ChromagramContext_Free(ChromagramContext *ctx);

// Compute HPCP for a frame of mono audio, using IF refinement when
// previous phase data is available.
// sampleCount must equal the fftSize used in ChromagramContext_Init.
Chromagram ComputeHPCP(ChromagramContext *ctx,
                       const float *samples, int sampleCount, DWORD sampleRate);

#endif
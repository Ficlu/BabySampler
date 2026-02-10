// chromagram.h
//
// Harmonic Pitch Class Profile (HPCP) with Instantaneous Frequency
// refinement and harmonic folding.
//
// Standard HPCP maps FFT peaks to pitch classes using parabolic
// interpolation, which has ±1-2 Hz error. At low frequencies
// (e.g., C2 = 65 Hz, semitone width = 3.7 Hz), this error is
// large enough to misattribute pitch classes.
//
// This implementation adds two key refinements:
//
// 1. Phase-derivative Instantaneous Frequency (IF) estimation:
//    By comparing FFT bin phases across consecutive frames, we
//    recover exact sinusoidal frequencies with sub-Hz accuracy
//    regardless of FFT size.
//    Theory: Müller (2015) §8.2.1; Kodera et al. (1976); Auger & Flandrin (1995)
//
// 2. Harmonic folding (Gómez 2006):
//    Each detected spectral peak could be the fundamental, or the
//    2nd, 3rd, ... Nth harmonic of a lower fundamental. We distribute
//    each peak's energy across all candidate fundamentals with
//    exponentially decaying weight: λ^(h-1) for the h-th harmonic
//    hypothesis. This folds harmonic energy back toward fundamentals,
//    dramatically cleaning up the chromagram for real instruments.
//    Theory: Gómez, "Tonal Description of Music Audio Signals" (2006), §3.2

#ifndef CHROMAGRAM_H
#define CHROMAGRAM_H

#include <windows.h>

#define CHROMA_BINS 12

// ---- Frequency range configuration ----

// Valid range for candidate fundamentals (pitch class mapping).
// Only frequencies in this range produce pitch class entries.
#define HPCP_MIN_FUNDAMENTAL_FREQ  55.0f   // Just below A1
#define HPCP_MAX_FUNDAMENTAL_FREQ  2000.0f // Covers most musical content

// Upper limit for spectral peak detection.
// We scan peaks up to this frequency to capture harmonics of notes
// within the fundamental range. A 1000 Hz fundamental has its 5th
// harmonic at 5000 Hz; above that, harmonic energy is typically
// negligible and we'd mostly be picking up noise/transients.
//
// This must be > HPCP_MAX_FUNDAMENTAL_FREQ. Peaks between
// MAX_FUNDAMENTAL and MAX_PEAK are only used via harmonic folding
// (they never contribute as h=1 fundamentals above MAX_FUNDAMENTAL).
#define HPCP_MAX_PEAK_FREQ  5000.0f

// Reference frequency for pitch class mapping (A4)
#define HPCP_REF_FREQ 440.0f

// ---- Harmonic folding configuration ----

// Maximum harmonic number to consider.
// For each spectral peak at frequency f, we test hypotheses:
//   h=1: peak IS the fundamental at f
//   h=2: peak is the 2nd harmonic of a fundamental at f/2
//   ...
//   h=H: peak is the Hth harmonic of a fundamental at f/H
//
// 8 harmonics captures the musically significant partial series.
// Higher harmonics have negligible weight (λ^7 = 0.028 at λ=0.6)
// and would mostly add noise.
#define HPCP_MAX_HARMONIC  8

// Exponential decay factor for harmonic weighting (Gómez 2006).
// Weight for harmonic h = λ^(h-1):
//   h=1: 1.000  (fundamental — full weight)
//   h=2: 0.600  (octave harmonic)
//   h=3: 0.360  (perfect fifth + octave)
//   h=4: 0.216  (two octaves)
//   h=5: 0.130  (major third + two octaves)
//   h=6: 0.078
//   h=7: 0.047
//   h=8: 0.028
//
// λ = 0.6 is Gómez's default, validated against the MIREX key
// detection dataset. Lower values (0.4) give less harmonic folding;
// higher values (0.8) fold more aggressively (useful if harmonics
// are very strong, e.g., distorted guitar).
#define HPCP_HARMONIC_DECAY  0.6f

// ---- Thresholding ----

// Minimum peak magnitude relative to the frame's strongest peak.
// Peaks below this are ignored (noise rejection).
#define HPCP_RELATIVE_THRESHOLD 0.01f

// ---- Data structures ----

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

// Compute HPCP for a frame of mono audio.
// Uses IF refinement (when previous phase data is available) and
// harmonic folding to produce a clean pitch class energy distribution.
// sampleCount must equal the fftSize used in ChromagramContext_Init.
Chromagram ComputeHPCP(ChromagramContext *ctx,
                       const float *samples, int sampleCount, DWORD sampleRate);

#endif
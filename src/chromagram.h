// chromagram.h
//
// Harmonic Pitch Class Profile (HPCP) with three refinements:
//
// 1. Phase-derivative Instantaneous Frequency (IF) estimation:
//    Sub-Hz frequency accuracy at all octaves by comparing FFT phase
//    across consecutive frames.
//    Theory: Müller (2015) §8.2.1; Kodera et al. (1976)
//
// 2. Harmonic folding (Gómez 2006):
//    Each spectral peak's energy is distributed across candidate
//    fundamentals (freq/1, freq/2, ..., freq/H) with exponential
//    decay, folding harmonic series energy back to the fundamental.
//    Theory: Gómez (2006) §3.2
//
// 3. Spectral whitening:
//    Normalize each bin's magnitude by a local spectral envelope
//    before peak detection. This prevents sidelobes of strong peaks
//    from appearing as separate peaks, and equalizes the contribution
//    of notes at different loudness levels.
//    Theory: Gómez (2006) §3.1; Essentia SpectralWhitening

#ifndef CHROMAGRAM_H
#define CHROMAGRAM_H

#include <windows.h>

#define CHROMA_BINS 12

// ---- Frequency range configuration ----

// Valid range for candidate fundamentals (pitch class mapping).
#define HPCP_MIN_FUNDAMENTAL_FREQ  55.0f   // Just below A1
#define HPCP_MAX_FUNDAMENTAL_FREQ  2000.0f // Covers most musical content

// Upper limit for spectral peak detection (captures harmonics).
#define HPCP_MAX_PEAK_FREQ  5000.0f

// Reference frequency for pitch class mapping (A4)
#define HPCP_REF_FREQ 440.0f

// ---- Harmonic folding configuration ----

// Maximum harmonic number to consider per peak.
#define HPCP_MAX_HARMONIC  8

// Exponential decay factor for harmonic weighting.
// Weight for harmonic h = λ^(h-1).
#define HPCP_HARMONIC_DECAY  0.6f

// ---- Spectral whitening configuration ----

// Half-window size (in bins) for the moving-average spectral envelope.
//
// At 48kHz with 8192-point FFT, freq resolution = 5.86 Hz/bin.
// ±40 bins = ±234 Hz window. This is:
//   - Wide enough to smooth over individual peaks (BH main lobe ≈ 8 bins)
//     so a peak only contributes ~10% to its own envelope.
//   - Narrow enough to follow the gross spectral tilt, so whitening
//     doesn't over-normalize the low end vs high end.
//
// The envelope is used to normalize peak magnitudes before the local-
// maximum test. Peaks that are only prominent because of a strong
// neighbor's sidelobes get suppressed; genuine peaks that stand out
// above their local spectral floor are retained.
#define HPCP_WHITEN_HALF_WINDOW  40

// ---- Thresholding ----

// Minimum peak magnitude relative to the frame's strongest peak.
#define HPCP_RELATIVE_THRESHOLD 0.01f

// Minimum whitened magnitude for a bin to be considered a peak.
// After dividing by the local envelope, genuine peaks typically have
// whitened values > 2.0 (they're 2× the local average). Sidelobes
// and noise sit closer to 1.0. A threshold of 1.5 rejects most
// spurious peaks while keeping all real spectral content.
#define HPCP_WHITEN_PEAK_THRESHOLD  1.5f

// ---- Chroma bin assignment ----

// Use cosine-weighted distribution instead of hard quantization.
// A peak's energy is spread between the two nearest pitch classes
// using a raised cosine kernel:
//   w(d) = 0.5 * (1 - cos(pi * d))
// where d is the fractional distance (0-1) between adjacent bins.
// This handles detuned sources gracefully and eliminates the hard
// boundary where a peak at 261.1 Hz goes 100% to C but at 261.7 Hz
// might flip to C#.
// Ref: Gomez (2006) section 3.2

// ---- Tuning estimation ----

// Resolution of the tuning offset histogram in cents.
// 1 cent = 1/100th of a semitone. Range: +/-50 cents (one full semitone).
#define HPCP_TUNING_RESOLUTION  1
#define HPCP_TUNING_BINS        101   // -50 to +50 cents inclusive

// Minimum number of peaks before we trust the tuning estimate.
// Below this, we use the default reference frequency (440 Hz).
#define HPCP_TUNING_MIN_PEAKS   50

// Number of bins to smooth the tuning histogram (half-window).
// Smoothing suppresses noise in the offset distribution and gives
// a cleaner peak. 3 bins = +/-3 cents smoothing at 1-cent resolution.
#define HPCP_TUNING_SMOOTH      3

// ---- Data structures ----

// Result of HPCP analysis for one frame
typedef struct {
    float bins[CHROMA_BINS];  // Energy per pitch class (C=0, C#=1, ... B=11)
    float totalEnergy;        // Sum of all bin energies (after harmonic weighting)
    int   peakCount;          // Number of spectral peaks detected this frame
} Chromagram;

// Persistent context for phase-derivative IF estimation and tuning.
typedef struct {
    float *prevReal;     // Previous frame FFT real parts [specSize]
    float *prevImag;     // Previous frame FFT imaginary parts [specSize]
    int    specSize;     // N/2 (number of frequency bins)
    int    hopSize;      // Hop size in samples (for IF calculation)
    BOOL   hasPrev;      // FALSE until first frame has been processed

    // Tuning estimation state.
    // Accumulates fractional-semitone offsets of all detected peaks
    // into a 1-cent-resolution histogram. After enough peaks have been
    // seen, EstimateTuning() finds the mode and returns the offset in
    // cents, which is used to adjust the reference frequency.
    float  tuningHistogram[HPCP_TUNING_BINS];  // weighted offset counts
    int    tuningPeakCount;                     // total peaks accumulated
    float  tuningOffsetCents;                   // estimated offset (0 until computed)
    BOOL   tuningLocked;                        // TRUE once estimate is committed
} ChromagramContext;

// Initialize/free the IF context.
BOOL ChromagramContext_Init(ChromagramContext *ctx, int fftSize, int hopSize);
void ChromagramContext_Free(ChromagramContext *ctx);

// Compute HPCP for a frame of mono audio.
Chromagram ComputeHPCP(ChromagramContext *ctx,
                       const float *samples, int sampleCount, DWORD sampleRate);

// Estimate the global tuning offset from accumulated peak data.
// Returns the offset in cents (-50 to +50). Call after enough frames
// have been processed (check ctx->tuningPeakCount >= HPCP_TUNING_MIN_PEAKS).
// Once called, sets ctx->tuningLocked = TRUE and adjusts subsequent
// HPCP computations to use the corrected reference frequency.
float EstimateTuning(ChromagramContext *ctx);

#endif
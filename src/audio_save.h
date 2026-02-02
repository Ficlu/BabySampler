// audio_save.h
#ifndef AUDIO_SAVE_H
#define AUDIO_SAVE_H

#include <windows.h>
#include <stdio.h>
#include "audio_capture.h"

// Write a 16-bit PCM WAV header (for compatibility exports)
void WriteWavHeader(FILE *file, WAVEFORMATEX *pwfx, DWORD dataSize);

// Write a 32-bit float WAV header (for lossless saves)
void WriteWavHeaderFloat(FILE *file, DWORD sampleRate, WORD numChannels, DWORD dataSize);

#endif // AUDIO_SAVE_H
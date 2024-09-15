// audio_save.h
#ifndef AUDIO_SAVE_H
#define AUDIO_SAVE_H

#include <windows.h>
#include <stdio.h>
#include "audio_capture.h"

void WriteWavHeader(FILE *file, WAVEFORMATEX *pwfx, DWORD dataSize);

#endif // AUDIO_SAVE_H
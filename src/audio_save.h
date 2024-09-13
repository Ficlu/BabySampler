// audio_save.h
#ifndef AUDIO_SAVE_H
#define AUDIO_SAVE_H

#include <windows.h>
#include <stdio.h>
#include "audio_capture.h"

void WriteWavHeader(FILE *file, WAVEFORMATEX *pwfx);
void FinalizeWavFile(FILE *file, DWORD dataLength);

#endif // AUDIO_SAVE_H

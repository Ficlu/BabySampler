// audio_save.c
#include "audio_save.h"
void WriteWavHeader(FILE *file, WAVEFORMATEX *pwfx, DWORD dataSize) {
    DWORD fileSize = dataSize + 36; // Total file size minus 8 bytes for RIFF header
    DWORD fmtSize = 16;
    WORD  formatTag = WAVE_FORMAT_PCM; // Changed to PCM
    WORD  channels = pwfx->nChannels;
    DWORD sampleRate = pwfx->nSamplesPerSec;
    WORD  bitsPerSample = pwfx->wBitsPerSample;
    WORD  blockAlign = pwfx->nBlockAlign;
    DWORD byteRate = pwfx->nAvgBytesPerSec;

    fwrite("RIFF", 1, 4, file);
    fwrite(&fileSize, sizeof(DWORD), 1, file);
    fwrite("WAVE", 1, 4, file);
    fwrite("fmt ", 1, 4, file);
    fwrite(&fmtSize, sizeof(DWORD), 1, file);
    fwrite(&formatTag, sizeof(WORD), 1, file);
    fwrite(&channels, sizeof(WORD), 1, file);
    fwrite(&sampleRate, sizeof(DWORD), 1, file);
    fwrite(&byteRate, sizeof(DWORD), 1, file);
    fwrite(&blockAlign, sizeof(WORD), 1, file);
    fwrite(&bitsPerSample, sizeof(WORD), 1, file);
    fwrite("data", 1, 4, file);
    fwrite(&dataSize, sizeof(DWORD), 1, file);

    // Check if all writes were successful
    if (ferror(file)) {
        fprintf(stderr, "Error writing WAV header\n");
    }
}

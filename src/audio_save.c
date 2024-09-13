// audio_save.c
#include "audio_save.h"

void WriteWavHeader(FILE *file, WAVEFORMATEX *pwfx) {
    DWORD fileSize = 0; // Placeholder for file size
    DWORD fmtSize = 16;
    WORD  formatTag = WAVE_FORMAT_PCM;
    WORD  channels = pwfx->nChannels;
    DWORD sampleRate = pwfx->nSamplesPerSec;
    DWORD byteRate = pwfx->nAvgBytesPerSec;
    WORD  blockAlign = pwfx->nBlockAlign;
    WORD  bitsPerSample = pwfx->wBitsPerSample;

    fseek(file, 0, SEEK_SET);

    // 'RIFF' chunk descriptor
    fwrite("RIFF", 1, 4, file);
    fwrite(&fileSize, sizeof(DWORD), 1, file); // To be updated later
    fwrite("WAVE", 1, 4, file);

    // 'fmt ' sub-chunk
    fwrite("fmt ", 1, 4, file);
    fwrite(&fmtSize, sizeof(DWORD), 1, file);
    fwrite(&formatTag, sizeof(WORD), 1, file);
    fwrite(&channels, sizeof(WORD), 1, file);
    fwrite(&sampleRate, sizeof(DWORD), 1, file);
    fwrite(&byteRate, sizeof(DWORD), 1, file);
    fwrite(&blockAlign, sizeof(WORD), 1, file);
    fwrite(&bitsPerSample, sizeof(WORD), 1, file);

    // 'data' sub-chunk
    fwrite("data", 1, 4, file);
    fwrite(&fileSize, sizeof(DWORD), 1, file); // To be updated later
}

void FinalizeWavFile(FILE *file, DWORD dataLength) {
    DWORD fileSize = dataLength + 36;

    fseek(file, 4, SEEK_SET);
    fwrite(&fileSize, sizeof(DWORD), 1, file);
    fseek(file, 40, SEEK_SET);
    fwrite(&dataLength, sizeof(DWORD), 1, file);
}

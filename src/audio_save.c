// audio_save.c
#include "audio_save.h"

// WAVE_FORMAT_IEEE_FLOAT = 0x0003
#define WAVE_FORMAT_IEEE_FLOAT 0x0003

// Write a standard 16-bit PCM WAV header
void WriteWavHeader(FILE *file, WAVEFORMATEX *pwfx, DWORD dataSize) {
    DWORD fileSize = dataSize + 36;
    DWORD fmtSize = 16;
    WORD  formatTag = WAVE_FORMAT_PCM;
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

    if (ferror(file)) {
        fprintf(stderr, "Error writing WAV header\n");
    }
}

// Write a 32-bit IEEE float WAV header
// This format preserves the full precision of the captured audio
void WriteWavHeaderFloat(FILE *file, DWORD sampleRate, WORD numChannels, DWORD dataSize) {
    // For float WAV, we need:
    // - Format tag 0x0003 (IEEE float)
    // - 32 bits per sample
    // - Appropriate block align and byte rate
    
    WORD  formatTag = WAVE_FORMAT_IEEE_FLOAT;
    WORD  bitsPerSample = 32;
    WORD  blockAlign = numChannels * (bitsPerSample / 8);  // channels * 4 bytes
    DWORD byteRate = sampleRate * blockAlign;
    DWORD fmtSize = 16;
    DWORD fileSize = dataSize + 36;  // Total file size minus 8 bytes for RIFF header

    // RIFF header
    fwrite("RIFF", 1, 4, file);
    fwrite(&fileSize, sizeof(DWORD), 1, file);
    fwrite("WAVE", 1, 4, file);
    
    // fmt chunk
    fwrite("fmt ", 1, 4, file);
    fwrite(&fmtSize, sizeof(DWORD), 1, file);
    fwrite(&formatTag, sizeof(WORD), 1, file);
    fwrite(&numChannels, sizeof(WORD), 1, file);
    fwrite(&sampleRate, sizeof(DWORD), 1, file);
    fwrite(&byteRate, sizeof(DWORD), 1, file);
    fwrite(&blockAlign, sizeof(WORD), 1, file);
    fwrite(&bitsPerSample, sizeof(WORD), 1, file);
    
    // data chunk header
    fwrite("data", 1, 4, file);
    fwrite(&dataSize, sizeof(DWORD), 1, file);

    if (ferror(file)) {
        fprintf(stderr, "Error writing float WAV header\n");
    }
}
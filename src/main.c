// main.c
#include <windows.h>
#include <stdio.h>
#include <mmsystem.h>
#include <stdint.h>
#include "audio_capture.h"
#include "audio_save.h"
#include "gui.h"

#define INITIAL_BUFFER_SIZE (1024 * 1024)  // Start with 1MB buffer
#define BUFFER_GROWTH_FACTOR 2
#define MAX_RETRY_COUNT 3
#define RETRY_DELAY_MS 100

BOOL isRecording = FALSE;
BOOL isPlaying = FALSE;
AudioCaptureContext ctx = { 0 };
HWAVEOUT hWaveOut = NULL;
WAVEHDR waveHdr = {0};
BYTE *audioBuffer = NULL;
BYTE *playbackBuffer = NULL;
DWORD bufferSize = 0;
DWORD capturedBytes = 0;
DWORD playbackBufferSize = 0;
DWORD g_nSamplesPerSec = 0;
WORD g_nChannels = 0;

// Function prototypes
void PlayAudio(HWND hwnd);
void StopAudio();
void SaveAudio(HWND hwnd);

DWORD WINAPI RecordingThread(LPVOID lpParam)
{
    HWND hwnd = (HWND)lpParam;
    HRESULT hr;

    printf("Starting recording thread\n");

    hr = InitializeAudioCapture(&ctx);
    if (FAILED(hr)) {
        MessageBox(hwnd, "Failed to initialize audio capture", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_nSamplesPerSec = ctx.pwfx->nSamplesPerSec;
    g_nChannels = ctx.pwfx->nChannels;
    printf("Stored format: channels=%d, sample rate=%d\n", g_nChannels, g_nSamplesPerSec);

    bufferSize = INITIAL_BUFFER_SIZE;
    audioBuffer = (BYTE*)malloc(bufferSize);
    if (!audioBuffer) {
        MessageBox(hwnd, "Failed to allocate memory for audio buffer", "Error", MB_OK | MB_ICONERROR);
        CleanupAudioCapture(&ctx);
        return 1;
    }

    hr = StartAudioCapture(&ctx);
    if (FAILED(hr)) {
        MessageBox(hwnd, "Failed to start audio capture", "Error", MB_OK | MB_ICONERROR);
        CleanupAudioCapture(&ctx);
        free(audioBuffer);
        return 1;
    }

    printf("Audio capture started\n");

    capturedBytes = 0;
    while (isRecording) {
        Sleep(10);  // Sleep to prevent busy waiting

        UINT32 packetLength = 0;
        hr = ctx.pCaptureClient->lpVtbl->GetNextPacketSize(ctx.pCaptureClient, &packetLength);
        if (FAILED(hr)) break;

        while (packetLength != 0) {
            BYTE *pData;
            DWORD flags;

            hr = ctx.pCaptureClient->lpVtbl->GetBuffer(ctx.pCaptureClient, &pData, &packetLength, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            UINT32 frameCount = packetLength;
            UINT32 bytesPerFrame = ctx.blockAlign;
            UINT32 totalBytes = frameCount * bytesPerFrame;

            if (capturedBytes + totalBytes > bufferSize) {
                DWORD newBufferSize = bufferSize * BUFFER_GROWTH_FACTOR;
                BYTE *newBuffer = (BYTE*)realloc(audioBuffer, newBufferSize);
                if (!newBuffer) {
                    MessageBox(hwnd, "Failed to grow audio buffer", "Error", MB_OK | MB_ICONERROR);
                    isRecording = FALSE;
                    break;
                }
                audioBuffer = newBuffer;
                bufferSize = newBufferSize;
                printf("Grew audio buffer to %u bytes\n", bufferSize);
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                memset(audioBuffer + capturedBytes, 0, totalBytes);
            } else {
                memcpy(audioBuffer + capturedBytes, pData, totalBytes);
            }
            capturedBytes += totalBytes;

            hr = ctx.pCaptureClient->lpVtbl->ReleaseBuffer(ctx.pCaptureClient, frameCount);
            if (FAILED(hr)) break;

            hr = ctx.pCaptureClient->lpVtbl->GetNextPacketSize(ctx.pCaptureClient, &packetLength);
            if (FAILED(hr)) break;
        }

        if (FAILED(hr)) break;
    }

    printf("Recording stopped. Captured %u bytes\n", capturedBytes);

    ctx.pAudioClient->lpVtbl->Stop(ctx.pAudioClient);
    CleanupAudioCapture(&ctx);

    UpdateRecordingStatus(hwnd, FALSE);
    isRecording = FALSE;

    return 0;
}

void PlayAudio(HWND hwnd)
{
    printf("PlayAudio called\n");

    if (!audioBuffer || capturedBytes == 0 || g_nChannels == 0 || g_nSamplesPerSec == 0) {
        MessageBox(hwnd, "No valid audio data to play", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    StopAudio();
    Sleep(RETRY_DELAY_MS);

    int sampleCount = capturedBytes / sizeof(float);
    short *convertedBuffer = (short *)malloc(sampleCount * sizeof(short));
    if (!convertedBuffer) {
        MessageBox(hwnd, "Failed to allocate memory for playback", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    float *floatData = (float *)audioBuffer;
    for (int i = 0; i < sampleCount; ++i) {
        float sample = floatData[i];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        convertedBuffer[i] = (short)(sample * 32767);
    }

    playbackBufferSize = sampleCount * sizeof(short);
    playbackBuffer = (BYTE *)convertedBuffer;

    printf("Converted %d samples for playback\n", sampleCount);

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = g_nChannels;
    wfx.nSamplesPerSec = g_nSamplesPerSec;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    printf("Attempting to open with format: channels=%d, sample rate=%d, bits per sample=%d\n",
           wfx.nChannels, wfx.nSamplesPerSec, wfx.wBitsPerSample);

    MMRESULT result;
    int retryCount = 0;
    do {
        result = waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)hwnd, 0, CALLBACK_WINDOW);
        if (result != MMSYSERR_NOERROR) {
            char errorMsg[256];
            waveOutGetErrorTextA(result, errorMsg, sizeof(errorMsg));
            printf("waveOutOpen failed. Error: %s (code %d), retrying...\n", errorMsg, result);
            Sleep(RETRY_DELAY_MS);
            retryCount++;
        }
    } while (result != MMSYSERR_NOERROR && retryCount < MAX_RETRY_COUNT);

    if (result != MMSYSERR_NOERROR) {
        char errorMsg[256];
        waveOutGetErrorTextA(result, errorMsg, sizeof(errorMsg));
        char fullErrorMsg[512];
        snprintf(fullErrorMsg, sizeof(fullErrorMsg), "Failed to open audio output device.\nError: %s (code %d)", errorMsg, result);
        MessageBox(hwnd, fullErrorMsg, "Error", MB_OK | MB_ICONERROR);
        free(playbackBuffer);
        playbackBuffer = NULL;
        return;
    }

    printf("waveOutOpen succeeded\n");

    memset(&waveHdr, 0, sizeof(WAVEHDR));
    waveHdr.lpData = (LPSTR)playbackBuffer;
    waveHdr.dwBufferLength = playbackBufferSize;
    waveHdr.dwFlags = 0;

    result = waveOutPrepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        char errorMsg[256];
        waveOutGetErrorTextA(result, errorMsg, sizeof(errorMsg));
        printf("Failed to prepare audio header. Error: %s (code %d)\n", errorMsg, result);
        waveOutClose(hWaveOut);
        free(playbackBuffer);
        playbackBuffer = NULL;
        hWaveOut = NULL;
        return;
    }

    printf("waveOutPrepareHeader succeeded\n");

    result = waveOutWrite(hWaveOut, &waveHdr, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        char errorMsg[256];
        waveOutGetErrorTextA(result, errorMsg, sizeof(errorMsg));
        printf("Failed to write audio data. Error: %s (code %d)\n", errorMsg, result);
        waveOutUnprepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
        waveOutClose(hWaveOut);
        free(playbackBuffer);
        playbackBuffer = NULL;
        hWaveOut = NULL;
        return;
    }

    printf("waveOutWrite succeeded\n");

    isPlaying = TRUE;
    UpdatePlayStatus(isPlaying);
}

void StopAudio()
{
    printf("StopAudio called. isPlaying: %d, hWaveOut: %p\n", isPlaying, (void*)hWaveOut);

    if (isPlaying || hWaveOut) {
        if (hWaveOut) {
            MMRESULT result;

            result = waveOutReset(hWaveOut);
            printf("waveOutReset result: %d\n", result);

            result = waveOutUnprepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
            printf("waveOutUnprepareHeader result: %d\n", result);

            result = waveOutClose(hWaveOut);
            printf("waveOutClose result: %d\n", result);

            hWaveOut = NULL;
        }

        if (playbackBuffer) {
            free(playbackBuffer);
            playbackBuffer = NULL;
            printf("Freed playback buffer\n");
        }

        memset(&waveHdr, 0, sizeof(WAVEHDR));

        isPlaying = FALSE;
        UpdatePlayStatus(isPlaying);
    }
}

void SaveAudio(HWND hwnd)
{
    printf("SaveAudio called\n");

    if (!audioBuffer || capturedBytes == 0 || g_nChannels == 0 || g_nSamplesPerSec == 0) {
        MessageBox(hwnd, "No valid audio data to save", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    StopAudio();

    FILE *file = fopen("output.wav", "wb");
    if (!file) {
        MessageBox(hwnd, "Failed to open output.wav for writing", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    int sampleCount = capturedBytes / sizeof(float);
    short *convertedBuffer = (short *)malloc(sampleCount * sizeof(short));
    if (!convertedBuffer) {
        MessageBox(hwnd, "Failed to allocate memory for saving", "Error", MB_OK | MB_ICONERROR);
        fclose(file);
        return;
    }

    float *floatData = (float *)audioBuffer;
    for (int i = 0; i < sampleCount; ++i) {
        float sample = floatData[i];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        convertedBuffer[i] = (short)(sample * 32767);
    }

    DWORD dataSize = sampleCount * sizeof(short);

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = g_nChannels;
    wfx.nSamplesPerSec = g_nSamplesPerSec;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    WriteWavHeader(file, &wfx, dataSize);

    size_t written = fwrite(convertedBuffer, 1, dataSize, file);
    free(convertedBuffer);

    if (written != dataSize) {
        char errorMsg[256];
        snprintf(errorMsg, sizeof(errorMsg), "Failed to write all audio data. Written: %zu, Expected: %lu", written, dataSize);
        MessageBox(hwnd, errorMsg, "Write Error", MB_OK | MB_ICONERROR);
        fclose(file);
        return;
    }

    fclose(file);

    printf("Audio saved successfully\n");
    MessageBox(hwnd, "Audio saved successfully", "Success", MB_OK | MB_ICONINFORMATION);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    printf("Application started\n");

    HWND hwnd = InitializeGUI(hInstance, nCmdShow);
    if (hwnd == NULL) {
        printf("Failed to initialize GUI\n");
        return 0;
    }

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (msg.message == WM_USER + 1) // Start recording
        {
            printf("Received Start Recording message\n");
            if (!isRecording)
            {
                isRecording = TRUE;
                UpdateRecordingStatus(hwnd, TRUE);
                CreateThread(NULL, 0, RecordingThread, hwnd, 0, NULL);
            }
        }
        else if (msg.message == WM_USER + 2) // Stop recording
        {
            printf("Received Stop Recording message\n");
            if (isRecording)
            {
                isRecording = FALSE;
            }
        }
        else if (msg.message == WM_USER + 3) // Play/Stop audio
        {
            printf("Received Play/Stop Audio message\n");
if (!isPlaying)
            {
                PlayAudio(hwnd);
            }
            else
            {
                StopAudio();
            }
        }
        else if (msg.message == WM_USER + 4) // Save audio
        {
            printf("Received Save Audio message\n");
            SaveAudio(hwnd);
        }
        else if (msg.message == MM_WOM_DONE) // Audio playback finished
        {
            printf("Received MM_WOM_DONE message. hWaveOut: %p\n", (void*)hWaveOut);
            if (hWaveOut) {
                MMRESULT result = waveOutUnprepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
                printf("waveOutUnprepareHeader result: %d\n", result);

                result = waveOutClose(hWaveOut);
                printf("waveOutClose result: %d\n", result);
                hWaveOut = NULL;
            }

            if (playbackBuffer) {
                free(playbackBuffer);
                playbackBuffer = NULL;
                printf("Freed playback buffer\n");
            }

            memset(&waveHdr, 0, sizeof(WAVEHDR));

            isPlaying = FALSE;
            UpdatePlayStatus(isPlaying);
        }
        else
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // Free resources before exiting
    if (audioBuffer) {
        free(audioBuffer);
        audioBuffer = NULL;
        printf("Freed audio buffer\n");
    }
    if (playbackBuffer) {
        free(playbackBuffer);
        playbackBuffer = NULL;
        printf("Freed playback buffer\n");
    }

    printf("Application exiting\n");
    return 0;
}
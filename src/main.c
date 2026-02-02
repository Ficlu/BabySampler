// main.c
#include <windows.h>
#include <stdio.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
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

// Dithering state - we keep previous random value for TPDF
static float ditherState = 0.0f;

// Function prototypes
void PlayAudio(HWND hwnd);
void StopAudio();
void SaveAudio(HWND hwnd);
float CalculatePeakLevel(const float *samples, int sampleCount);
float CalculateBufferPeak(const BYTE *buffer, DWORD byteCount);

// Generate a random float between -1.0 and 1.0
static inline float RandomFloat()
{
    return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
}

// Calculate peak level from float samples
// Returns the maximum absolute value found
float CalculatePeakLevel(const float *samples, int sampleCount)
{
    float peak = 0.0f;
    for (int i = 0; i < sampleCount; i++)
    {
        float absVal = fabsf(samples[i]);
        if (absVal > peak)
        {
            peak = absVal;
        }
    }
    return peak;
}

// Calculate peak from the entire captured buffer
float CalculateBufferPeak(const BYTE *buffer, DWORD byteCount)
{
    int sampleCount = byteCount / sizeof(float);
    return CalculatePeakLevel((const float *)buffer, sampleCount);
}

// Convert float samples to 16-bit PCM with TPDF dithering
// TPDF (Triangular Probability Density Function) dithering uses the sum of two 
// uniform random values, which creates a triangular distribution. This effectively
// decorrelates quantization error from the signal, replacing distortion with a 
// constant, low-level noise floor.
void ConvertFloatTo16BitWithDither(const float *input, short *output, int sampleCount)
{
    const float scale = 32767.0f;
    const float ditherAmp = 1.0f / scale;  // 1 LSB worth of dither
    
    for (int i = 0; i < sampleCount; i++)
    {
        float sample = input[i];
        
        // Soft clipping using tanh for samples that exceed range
        // This sounds more natural than hard clipping
        if (sample > 1.0f || sample < -1.0f)
        {
            sample = tanhf(sample);
        }
        
        // TPDF dither: sum of two uniform random values gives triangular distribution
        float newRandom = RandomFloat();
        float dither = (ditherState + newRandom) * ditherAmp;
        ditherState = newRandom;
        
        // Scale to 16-bit range and add dither
        float scaled = sample * scale + dither;
        
        // Round to nearest integer (not truncate)
        int32_t rounded;
        if (scaled >= 0.0f)
            rounded = (int32_t)(scaled + 0.5f);
        else
            rounded = (int32_t)(scaled - 0.5f);
        
        // Clamp to valid 16-bit range
        if (rounded > 32767)
            rounded = 32767;
        else if (rounded < -32768)
            rounded = -32768;
        
        output[i] = (short)rounded;
    }
}

DWORD WINAPI RecordingThread(LPVOID lpParam)
{
    HWND hwnd = (HWND)lpParam;
    HRESULT hr;
    float sessionPeak = 0.0f;  // Track peak across entire recording
    DWORD lastMeterUpdate = 0;
    const DWORD METER_UPDATE_INTERVAL = 50;  // Update meter every 50ms

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

        float packetPeak = 0.0f;  // Track peak for this batch of packets

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
                
                // Calculate peak for this packet
                int sampleCount = frameCount * ctx.pwfx->nChannels;
                float peak = CalculatePeakLevel((float *)pData, sampleCount);
                if (peak > packetPeak) packetPeak = peak;
                if (peak > sessionPeak) sessionPeak = peak;
            }
            capturedBytes += totalBytes;

            hr = ctx.pCaptureClient->lpVtbl->ReleaseBuffer(ctx.pCaptureClient, frameCount);
            if (FAILED(hr)) break;

            hr = ctx.pCaptureClient->lpVtbl->GetNextPacketSize(ctx.pCaptureClient, &packetLength);
            if (FAILED(hr)) break;
        }

        // Update the peak meter at regular intervals (not every packet)
        DWORD now = GetTickCount();
        if (now - lastMeterUpdate >= METER_UPDATE_INTERVAL) {
            // Send peak value to GUI (multiply by 10000 to preserve precision as integer)
            PostMessage(hwnd, WM_UPDATE_PEAK, (WPARAM)(packetPeak * 10000.0f), 0);
            lastMeterUpdate = now;
        }

        if (FAILED(hr)) break;
    }

    printf("Recording stopped. Captured %u bytes\n", capturedBytes);
    printf("Session peak level: %.4f (%.1f dB)\n", sessionPeak, 
           sessionPeak > 0 ? 20.0f * log10f(sessionPeak) : -96.0f);

    // Send final peak reading
    PostMessage(hwnd, WM_UPDATE_PEAK, (WPARAM)(sessionPeak * 10000.0f), 0);

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

    // Calculate and display peak of captured buffer
    float bufferPeak = CalculateBufferPeak(audioBuffer, capturedBytes);
    printf("Playback buffer peak: %.4f (%.1f dB)\n", bufferPeak,
           bufferPeak > 0 ? 20.0f * log10f(bufferPeak) : -96.0f);
    PostMessage(hwnd, WM_UPDATE_PEAK, (WPARAM)(bufferPeak * 10000.0f), 0);

    int sampleCount = capturedBytes / sizeof(float);
    short *convertedBuffer = (short *)malloc(sampleCount * sizeof(short));
    if (!convertedBuffer) {
        MessageBox(hwnd, "Failed to allocate memory for playback", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Use improved conversion with dithering
    ConvertFloatTo16BitWithDither((float *)audioBuffer, convertedBuffer, sampleCount);

    playbackBufferSize = sampleCount * sizeof(short);
    playbackBuffer = (BYTE *)convertedBuffer;

    printf("Converted %d samples for playback (with TPDF dithering)\n", sampleCount);

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

    // Calculate and display peak of buffer being saved
    float bufferPeak = CalculateBufferPeak(audioBuffer, capturedBytes);
    printf("Saving buffer peak: %.4f (%.1f dB)\n", bufferPeak,
           bufferPeak > 0 ? 20.0f * log10f(bufferPeak) : -96.0f);

    FILE *file = fopen("output.wav", "wb");
    if (!file) {
        MessageBox(hwnd, "Failed to open output.wav for writing", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Write 32-bit float WAV header - no conversion needed
    DWORD dataSize = capturedBytes;
    WriteWavHeaderFloat(file, g_nSamplesPerSec, g_nChannels, dataSize);

    // Write the raw float audio buffer directly - bit-perfect copy
    size_t written = fwrite(audioBuffer, 1, dataSize, file);

    if (written != dataSize) {
        char errorMsg[256];
        snprintf(errorMsg, sizeof(errorMsg), "Failed to write all audio data. Written: %zu, Expected: %lu", written, dataSize);
        MessageBox(hwnd, errorMsg, "Write Error", MB_OK | MB_ICONERROR);
        fclose(file);
        return;
    }

    fclose(file);

    printf("Audio saved successfully (32-bit float, lossless)\n");
    MessageBox(hwnd, "Audio saved successfully (32-bit float)", "Success", MB_OK | MB_ICONINFORMATION);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    printf("Application started\n");

    // Seed random number generator for dithering
    srand((unsigned int)time(NULL));

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
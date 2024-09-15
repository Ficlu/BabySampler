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

// Function prototypes
void PlayAudio(HWND hwnd);
void StopAudio();
void SaveAudio(HWND hwnd);

// Debug logging function
void DebugLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    OutputDebugStringA(buffer);
}

DWORD WINAPI RecordingThread(LPVOID lpParam)
{
    HWND hwnd = (HWND)lpParam;
    HRESULT hr;

    DebugLog("Starting recording thread\n");

    hr = InitializeAudioCapture(&ctx);
    if (FAILED(hr)) {
        MessageBox(hwnd, "Failed to initialize audio capture", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

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

    DebugLog("Audio capture started\n");

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
                // Grow the buffer
                DWORD newBufferSize = bufferSize * BUFFER_GROWTH_FACTOR;
                BYTE *newBuffer = (BYTE*)realloc(audioBuffer, newBufferSize);
                if (!newBuffer) {
                    MessageBox(hwnd, "Failed to grow audio buffer", "Error", MB_OK | MB_ICONERROR);
                    isRecording = FALSE;
                    break;
                }
                audioBuffer = newBuffer;
                bufferSize = newBufferSize;
                DebugLog("Grew audio buffer to %u bytes\n", bufferSize);
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

    DebugLog("Recording stopped. Captured %u bytes\n", capturedBytes);

    ctx.pAudioClient->lpVtbl->Stop(ctx.pAudioClient);
    CleanupAudioCapture(&ctx);

    UpdateRecordingStatus(hwnd, FALSE);
    isRecording = FALSE;

    return 0;
}

void PlayAudio(HWND hwnd)
{
    DebugLog("PlayAudio called\n");

    if (!audioBuffer || capturedBytes == 0) {
        MessageBox(hwnd, "No audio data to play", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Stop any ongoing playback
    StopAudio();

    // Add a small delay before trying to open the device again
    Sleep(RETRY_DELAY_MS);

    // Convert audio data to 16-bit PCM
    int sampleCount = capturedBytes / sizeof(float); // Assuming audioBuffer contains float samples
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
    playbackBuffer = (BYTE *)convertedBuffer; // Store the converted buffer in playbackBuffer

    DebugLog("Converted %d samples for playback\n", sampleCount);

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = ctx.pwfx->nChannels;
    wfx.nSamplesPerSec = ctx.pwfx->nSamplesPerSec;
    wfx.wBitsPerSample = 16;  // 16-bit PCM
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT result;
    int retryCount = 0;
    do {
        result = waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)hwnd, 0, CALLBACK_WINDOW);
        if (result != MMSYSERR_NOERROR) {
            DebugLog("waveOutOpen failed with error code %d, retrying...\n", result);
            Sleep(RETRY_DELAY_MS);
            retryCount++;
        }
    } while (result != MMSYSERR_NOERROR && retryCount < MAX_RETRY_COUNT);

    if (result != MMSYSERR_NOERROR) {
        char errorMsg[256];
        snprintf(errorMsg, sizeof(errorMsg), "Failed to open audio output device. Error code: %d", result);
        MessageBox(hwnd, errorMsg, "Error", MB_OK | MB_ICONERROR);
        free(playbackBuffer);
        playbackBuffer = NULL;
        return;
    }

    DebugLog("waveOutOpen succeeded\n");

    // Reset the waveHdr structure
    memset(&waveHdr, 0, sizeof(WAVEHDR));

    // Prepare the wave header
    waveHdr.lpData = (LPSTR)playbackBuffer;
    waveHdr.dwBufferLength = playbackBufferSize;
    waveHdr.dwFlags = 0;

    result = waveOutPrepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        char errorMsg[256];
        snprintf(errorMsg, sizeof(errorMsg), "Failed to prepare audio header. Error code: %d", result);
        MessageBox(hwnd, errorMsg, "Error", MB_OK | MB_ICONERROR);
        waveOutClose(hWaveOut);
        free(playbackBuffer);
        playbackBuffer = NULL;
        return;
    }

    DebugLog("waveOutPrepareHeader succeeded\n");

    result = waveOutWrite(hWaveOut, &waveHdr, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        char errorMsg[256];
        snprintf(errorMsg, sizeof(errorMsg), "Failed to write audio data. Error code: %d", result);
        MessageBox(hwnd, errorMsg, "Error", MB_OK | MB_ICONERROR);
        waveOutUnprepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
        waveOutClose(hWaveOut);
        free(playbackBuffer);
        playbackBuffer = NULL;
        return;
    }

    DebugLog("waveOutWrite succeeded\n");

    isPlaying = TRUE;
    UpdatePlayStatus(isPlaying);
}

void StopAudio()
{
    DebugLog("StopAudio called\n");

    if (isPlaying) {
        if (hWaveOut) {
            // Stop playback
            DebugLog("Stopping playback\n");
            waveOutReset(hWaveOut);

            // Unprepare the header
            MMRESULT result = waveOutUnprepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                DebugLog("Failed to unprepare audio header. Error code: %d\n", result);
            } else {
                DebugLog("waveOutUnprepareHeader succeeded\n");
            }

            // Close the device
            result = waveOutClose(hWaveOut);
            if (result != MMSYSERR_NOERROR) {
                DebugLog("Failed to close audio device. Error code: %d\n", result);
            } else {
                DebugLog("waveOutClose succeeded\n");
            }
            hWaveOut = NULL;
        }

        // Free the playback buffer
        if (playbackBuffer) {
            free(playbackBuffer);
            playbackBuffer = NULL;
            DebugLog("Freed playback buffer\n");
        }

        // Reset the wave header
        memset(&waveHdr, 0, sizeof(WAVEHDR));

        isPlaying = FALSE;
        UpdatePlayStatus(isPlaying);
    } else {
        DebugLog("StopAudio called but isPlaying was already FALSE\n");
    }
}

void SaveAudio(HWND hwnd)
{
    DebugLog("SaveAudio called\n");

    if (!audioBuffer || capturedBytes == 0) {
        MessageBox(hwnd, "No audio data to save", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Stop any ongoing playback
    StopAudio();

    FILE *file = fopen("output.wav", "wb");
    if (!file) {
        MessageBox(hwnd, "Failed to open output.wav for writing", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Convert audio data to 16-bit PCM
    int sampleCount = capturedBytes / sizeof(float); // Assuming audioBuffer contains float samples
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
    wfx.nChannels = ctx.pwfx->nChannels;
    wfx.nSamplesPerSec = ctx.pwfx->nSamplesPerSec;
    wfx.wBitsPerSample = 16;  // 16-bit PCM
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    // Write WAV header
    WriteWavHeader(file, &wfx, dataSize);

    // Write audio data
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

    DebugLog("Audio saved successfully\n");
    MessageBox(hwnd, "Audio saved successfully", "Success", MB_OK | MB_ICONINFORMATION);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    DebugLog("Application started\n");

    HWND hwnd = InitializeGUI(hInstance, nCmdShow);
    if (hwnd == NULL) {
        DebugLog("Failed to initialize GUI\n");
        return 0;
    }

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (msg.message == WM_USER + 1) // Start recording
        {
            DebugLog("Received Start Recording message\n");
            if (!isRecording)
            {
                isRecording = TRUE;
                UpdateRecordingStatus(hwnd, TRUE);
                CreateThread(NULL, 0, RecordingThread, hwnd, 0, NULL);
            }
        }
        else if (msg.message == WM_USER + 2) // Stop recording
        {
            DebugLog("Received Stop Recording message\n");
            if (isRecording)
            {
                isRecording = FALSE;
            }
        }
        else if (msg.message == WM_USER + 3) // Play/Stop audio
        {
            DebugLog("Received Play/Stop Audio message\n");
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
            DebugLog("Received Save Audio message\n");
            SaveAudio(hwnd);
        }
        else if (msg.message == MM_WOM_DONE) // Audio playback finished
        {
            DebugLog("Received MM_WOM_DONE message\n");
            if (hWaveOut) {
                // Unprepare the header
                MMRESULT result = waveOutUnprepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
                if (result != MMSYSERR_NOERROR) {
                    DebugLog("Failed to unprepare audio header. Error code: %d\n", result);
                } else {
                    DebugLog("waveOutUnprepareHeader succeeded\n");
                }

                // Close the device
                result = waveOutClose(hWaveOut);
                if (result != MMSYSERR_NOERROR) {
                    DebugLog("Failed to close audio device. Error code: %d\n", result);
                } else {
                    DebugLog("waveOutClose succeeded\n");
                }
                hWaveOut = NULL;
            }

            // Free the playback buffer
            if (playbackBuffer) {
                free(playbackBuffer);
                playbackBuffer = NULL;
                DebugLog("Freed playback buffer\n");
            }

            // Reset the wave header
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
        DebugLog("Freed audio buffer\n");
    }
    if (playbackBuffer) {
        free(playbackBuffer);
        playbackBuffer = NULL;
        DebugLog("Freed playback buffer\n");
    }

    DebugLog("Application exiting\n");
    return 0;
}
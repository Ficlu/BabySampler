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
#include "recording_list.h"
#include "pitch_detect.h"
#include "scale_detect.h"
#include "chromagram.h"

#define INITIAL_BUFFER_SIZE (1024 * 1024)  // Start with 1MB buffer
#define BUFFER_GROWTH_FACTOR 2
#define MAX_RETRY_COUNT 3
#define RETRY_DELAY_MS 100

// Fixed pitch analysis window size.
// 4096 samples at 48kHz = ~85ms — long enough for ~7 full periods at 80Hz
// (the lowest frequency we detect), which gives YIN plenty of data to work with.
// YIN's maxLag at 80Hz is 600 samples, and W = N/2 = 2048, so the integration
// window is more than 3x the maximum lag — well above the minimum requirement.
#define PITCH_WINDOW_SAMPLES 4096

// State
BOOL isRecording = FALSE;
BOOL isPlaying = FALSE;
AudioCaptureContext ctx = { 0 };
HWAVEOUT hWaveOut = NULL;
WAVEHDR waveHdr = {0};
BYTE *playbackBuffer = NULL;
DWORD playbackBufferSize = 0;

// Recording list - replaces single audioBuffer
RecordingList recordings = {0};

// Temporary buffer used during recording (ownership transfers to list when done)
BYTE *tempRecordBuffer = NULL;
DWORD tempBufferSize = 0;
DWORD tempCapturedBytes = 0;
DWORD tempSampleRate = 0;
WORD tempChannels = 0;
float tempSessionPeak = 0.0f;

// Dithering state
static float ditherState = 0.0f;

// Function prototypes
void PlayAudio(HWND hwnd);
void StopAudio();
void SaveAudio(HWND hwnd);
void DeleteRecording(HWND hwnd);
float CalculatePeakLevel(const float *samples, int sampleCount);
float CalculateBufferPeak(const BYTE *buffer, DWORD byteCount);

// Generate a random float between -1.0 and 1.0
static inline float RandomFloat()
{
    return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
}

// Calculate peak level from float samples
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

// Calculate peak from buffer
float CalculateBufferPeak(const BYTE *buffer, DWORD byteCount)
{
    int sampleCount = byteCount / sizeof(float);
    return CalculatePeakLevel((const float *)buffer, sampleCount);
}

// Convert float samples to 16-bit PCM with TPDF dithering
void ConvertFloatTo16BitWithDither(const float *input, short *output, int sampleCount)
{
    const float scale = 32767.0f;
    const float ditherAmp = 1.0f / scale;

    for (int i = 0; i < sampleCount; i++)
    {
        float sample = input[i];

        if (sample > 1.0f || sample < -1.0f)
        {
            sample = tanhf(sample);
        }

        float newRandom = RandomFloat();
        float dither = (ditherState + newRandom) * ditherAmp;
        ditherState = newRandom;

        float scaled = sample * scale + dither;

        int32_t rounded;
        if (scaled >= 0.0f)
            rounded = (int32_t)(scaled + 0.5f);
        else
            rounded = (int32_t)(scaled - 0.5f);

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
    DWORD lastMeterUpdate = 0;
    const DWORD METER_UPDATE_INTERVAL = 50;

    printf("Starting recording thread\n");

    hr = InitializeAudioCapture(&ctx);
    if (FAILED(hr)) {
        MessageBox(hwnd, "Failed to initialize audio capture", "Error", MB_OK | MB_ICONERROR);
        isRecording = FALSE;
        UpdateRecordingStatus(hwnd, FALSE);
        return 1;
    }

    tempSampleRate = ctx.pwfx->nSamplesPerSec;
    tempChannels = ctx.pwfx->nChannels;
    printf("Capture format: channels=%d, sample rate=%lu\n", tempChannels, tempSampleRate);

    // Initialize pitch detection (using instrument mode for broader compatibility)
    PitchConfig pitchConfig;
    PitchConfig_InitInstrument(&pitchConfig, tempSampleRate);

    PitchTracker pitchTracker;
    PitchTracker_Init(&pitchTracker);

    ScaleAccumulator scaleAcc;
    ScaleAccumulator_Init(&scaleAcc, tempSampleRate);

    // Confidence output from YIN, used to weight scale histogram entries
    float pitchConfidence = 0.0f;

    // Ring buffer for mono samples — decouples WASAPI packet reading from
    // pitch analysis. WASAPI always writes ALL samples here, analysis reads
    // whenever a hop's worth of new data has arrived.
    //
    // Analysis window: 8192 samples (170ms at 48kHz).
    // Doubled from the original 4096 to improve low-frequency resolution:
    //   4096 → 11.7 Hz/bin → ~5 bins per semitone at 55 Hz (poor)
    //   8192 →  5.9 Hz/bin → ~10 bins per semitone at 55 Hz (adequate)
    // This lets HPCP's peak-picking correctly resolve adjacent semitones
    // even in the lowest octave of the detection range.
    //
    // Hop: 2048 samples (43ms) gives ~23 frames/sec — enough statistical
    // mass for K-K correlation while keeping good temporal density.
    int analysisSize = 8192;
    int hopSize = 2048;
    int ringCapacity = analysisSize * 4;
    float *ringBuffer = (float*)malloc(ringCapacity * sizeof(float));
    int ringHead = 0;          // next write position
    int ringCount = 0;         // total samples currently in ring buffer
    int newSamples = 0;        // samples written since last analysis

    // Phase-derivative IF context for sub-bin frequency accuracy.
    // Stores previous frame's FFT phase to compute instantaneous
    // frequency via inter-frame phase differences (Müller 2015, §8.2.1).
    ChromagramContext chromaCtx;
    ChromagramContext_Init(&chromaCtx, analysisSize, hopSize);

    // Linear buffer for extracting analysis windows from the ring buffer
    float *analysisBuffer = (float*)malloc(analysisSize * sizeof(float));

    tempBufferSize = INITIAL_BUFFER_SIZE;
    tempRecordBuffer = (BYTE*)malloc(tempBufferSize);
    if (!tempRecordBuffer || !ringBuffer || !analysisBuffer) {
        MessageBox(hwnd, "Failed to allocate memory for audio buffer", "Error", MB_OK | MB_ICONERROR);
        CleanupAudioCapture(&ctx);
        if (tempRecordBuffer) free(tempRecordBuffer);
        if (ringBuffer) free(ringBuffer);
        if (analysisBuffer) free(analysisBuffer);
        ChromagramContext_Free(&chromaCtx);
        tempRecordBuffer = NULL;
        isRecording = FALSE;
        UpdateRecordingStatus(hwnd, FALSE);
        return 1;
    }

    hr = StartAudioCapture(&ctx);
    if (FAILED(hr)) {
        MessageBox(hwnd, "Failed to start audio capture", "Error", MB_OK | MB_ICONERROR);
        CleanupAudioCapture(&ctx);
        free(tempRecordBuffer);
        free(ringBuffer);
        free(analysisBuffer);
        ChromagramContext_Free(&chromaCtx);
        tempRecordBuffer = NULL;
        isRecording = FALSE;
        UpdateRecordingStatus(hwnd, FALSE);
        return 1;
    }

    printf("Audio capture started\n");
    printf("HPCP+IF analysis window: %d samples (%.1f ms), hop: %d samples (%.1f ms) at %lu Hz\n",
           analysisSize, 1000.0f * analysisSize / tempSampleRate,
           hopSize, 1000.0f * hopSize / tempSampleRate, tempSampleRate);
    printf("Frequency resolution: %.1f Hz/bin, IF refinement: sub-Hz precision at all frequencies\n",
           (float)tempSampleRate / (float)analysisSize);

    tempCapturedBytes = 0;
    tempSessionPeak = 0.0f;

    while (isRecording) {
        Sleep(10);

        UINT32 packetLength = 0;
        hr = ctx.pCaptureClient->lpVtbl->GetNextPacketSize(ctx.pCaptureClient, &packetLength);
        if (FAILED(hr)) break;

        float packetPeak = 0.0f;

        while (packetLength != 0) {
            BYTE *pData;
            DWORD flags;

            hr = ctx.pCaptureClient->lpVtbl->GetBuffer(ctx.pCaptureClient, &pData, &packetLength, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            UINT32 frameCount = packetLength;
            UINT32 bytesPerFrame = ctx.blockAlign;
            UINT32 totalBytes = frameCount * bytesPerFrame;

            if (tempCapturedBytes + totalBytes > tempBufferSize) {
                DWORD newBufferSize = tempBufferSize * BUFFER_GROWTH_FACTOR;
                BYTE *newBuffer = (BYTE*)realloc(tempRecordBuffer, newBufferSize);
                if (!newBuffer) {
                    MessageBox(hwnd, "Failed to grow audio buffer", "Error", MB_OK | MB_ICONERROR);
                    isRecording = FALSE;
                    break;
                }
                tempRecordBuffer = newBuffer;
                tempBufferSize = newBufferSize;
                printf("Grew audio buffer to %lu bytes\n", tempBufferSize);
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                memset(tempRecordBuffer + tempCapturedBytes, 0, totalBytes);
            } else {
                memcpy(tempRecordBuffer + tempCapturedBytes, pData, totalBytes);

                int sampleCount = frameCount * ctx.pwfx->nChannels;
                float peak = CalculatePeakLevel((float *)pData, sampleCount);
                if (peak > packetPeak) packetPeak = peak;
                if (peak > tempSessionPeak) tempSessionPeak = peak;

                // Downmix all frames to mono and write to ring buffer.
                // Every sample goes in — nothing is ever dropped.
                float *floatData = (float *)pData;
                for (UINT32 i = 0; i < frameCount; i++) {
                    float mono = 0.0f;
                    for (WORD ch = 0; ch < tempChannels; ch++) {
                        mono += floatData[i * tempChannels + ch];
                    }
                    mono /= tempChannels;
                    ringBuffer[ringHead] = mono;
                    ringHead = (ringHead + 1) % ringCapacity;
                    if (ringCount < ringCapacity) ringCount++;
                    newSamples++;
                }
            }
            tempCapturedBytes += totalBytes;

            hr = ctx.pCaptureClient->lpVtbl->ReleaseBuffer(ctx.pCaptureClient, frameCount);
            if (FAILED(hr)) break;

            hr = ctx.pCaptureClient->lpVtbl->GetNextPacketSize(ctx.pCaptureClient, &packetLength);
            if (FAILED(hr)) break;
        }

        DWORD now = GetTickCount();

        // Update peak meter
        if (now - lastMeterUpdate >= METER_UPDATE_INTERVAL) {
            PostMessage(hwnd, WM_UPDATE_PEAK, (WPARAM)(packetPeak * 10000.0f), 0);
            lastMeterUpdate = now;
        }

        // Run pitch analysis when a hop's worth of new samples has arrived.
        while (newSamples >= hopSize && ringCount >= analysisSize) {
            // Extract last analysisSize samples from ring buffer
            int start = (ringHead - analysisSize + ringCapacity) % ringCapacity;
            for (int i = 0; i < analysisSize; i++) {
                analysisBuffer[i] = ringBuffer[(start + i) % ringCapacity];
            }

            // Frame stability gate: skip windows that straddle note
            // transitions. Transition frames produce phantom peaks from
            // the mix of two frequencies, polluting the chromagram.
            BOOL stable = IsFrameStable(analysisBuffer, analysisSize, 6.0f);

            // Always compute HPCP to keep the phase context fresh for IF
            // estimation (IF needs consecutive frames' phases, even if we
            // don't use the chromagram result from unstable frames).
            Chromagram chroma = ComputeHPCP(&chromaCtx, analysisBuffer, analysisSize, tempSampleRate);

            // Only accumulate chromagram from stable frames
            if (stable && chroma.totalEnergy > 0.0f) {
                ScaleAccumulator_AddChromagram(&scaleAcc, chroma.bins);
            }

            // YIN for monophonic note display
            if (stable) {
                int pitchClass = DetectPitchTracked(analysisBuffer, analysisSize,
                                                     &pitchConfig, &pitchTracker,
                                                     &pitchConfidence);
                (void)pitchClass;
            }

            newSamples -= hopSize;
        }

        if (FAILED(hr)) break;
    }

    printf("Recording stopped. Captured %lu bytes\n", tempCapturedBytes);
    printf("Session peak level: %.4f (%.1f dB)\n", tempSessionPeak,
           tempSessionPeak > 0 ? 20.0f * log10f(tempSessionPeak) : -96.0f);

    // Analyze scale
    ScaleResult scaleResult = ScaleAccumulator_Analyze(&scaleAcc);
    char scaleName[32];
    ScaleResult_GetName(&scaleResult, scaleName, sizeof(scaleName));
    printf("Detected scale: %s (confidence: %.2f, notes: %d)\n",
           scaleName, scaleResult.confidence, scaleResult.totalNotes);

    // Print pitch histogram for debugging (HPCP energy-weighted)
    printf("HPCP histogram: ");
    const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    
    // Normalize for display: show as percentage of total energy
    float totalHpcpEnergy = 0.0f;
    for (int i = 0; i < 12; i++) {
        totalHpcpEnergy += scaleAcc.histogram[i];
    }
    
    for (int i = 0; i < 12; i++) {
        if (scaleAcc.histogram[i] > 0.0f && totalHpcpEnergy > 0.0f) {
            float pct = 100.0f * scaleAcc.histogram[i] / totalHpcpEnergy;
            printf("%s:%.1f%% ", noteNames[i], pct);
        }
    }
    printf("(%d frames)\n", scaleAcc.totalCount);

    PostMessage(hwnd, WM_UPDATE_PEAK, (WPARAM)(tempSessionPeak * 10000.0f), 0);

    ctx.pAudioClient->lpVtbl->Stop(ctx.pAudioClient);
    CleanupAudioCapture(&ctx);

    free(ringBuffer);
    free(analysisBuffer);
    ChromagramContext_Free(&chromaCtx);

    // Add the recording to the list (if we captured anything)
    if (tempCapturedBytes > 0 && tempRecordBuffer) {
        // Shrink buffer to actual size to save memory
        BYTE *finalBuffer = (BYTE*)realloc(tempRecordBuffer, tempCapturedBytes);
        if (finalBuffer) {
            tempRecordBuffer = finalBuffer;
        }

        int index = RecordingList_Add(&recordings, tempRecordBuffer, tempCapturedBytes,
                                      tempSampleRate, tempChannels, tempSessionPeak, &scaleResult);
        if (index >= 0) {
            printf("Added recording %d to list\n", index + 1);
            tempRecordBuffer = NULL;
        } else {
            printf("Failed to add recording to list\n");
            free(tempRecordBuffer);
            tempRecordBuffer = NULL;
        }
    } else if (tempRecordBuffer) {
        free(tempRecordBuffer);
        tempRecordBuffer = NULL;
    }

    // Notify main thread to refresh UI
    PostMessage(hwnd, WM_RECORDING_ADDED, 0, 0);

    UpdateRecordingStatus(hwnd, FALSE);
    isRecording = FALSE;

    return 0;
}

void PlayAudio(HWND hwnd)
{
    printf("PlayAudio called\n");

    int selCount = GetSelectedRecordingCount(hwnd);
    if (selCount == 0) {
        MessageBox(hwnd, "No recording selected", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    if (selCount > 1) {
        MessageBox(hwnd, "Select a single recording for playback", "Info", MB_OK | MB_ICONINFORMATION);
        return;
    }

    int index = GetSelectedRecordingIndex(hwnd);
    AudioRecording *rec = RecordingList_Get(&recordings, index);
    if (!rec || !rec->buffer || rec->size == 0) {
        MessageBox(hwnd, "Invalid recording selected", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    StopAudio();
    Sleep(RETRY_DELAY_MS);

    float bufferPeak = CalculateBufferPeak(rec->buffer, rec->size);
    printf("Playback buffer peak: %.4f (%.1f dB)\n", bufferPeak,
           bufferPeak > 0 ? 20.0f * log10f(bufferPeak) : -96.0f);
    PostMessage(hwnd, WM_UPDATE_PEAK, (WPARAM)(bufferPeak * 10000.0f), 0);

    int sampleCount = rec->size / sizeof(float);
    short *convertedBuffer = (short *)malloc(sampleCount * sizeof(short));
    if (!convertedBuffer) {
        MessageBox(hwnd, "Failed to allocate memory for playback", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    ConvertFloatTo16BitWithDither((float *)rec->buffer, convertedBuffer, sampleCount);

    playbackBufferSize = sampleCount * sizeof(short);
    playbackBuffer = (BYTE *)convertedBuffer;

    printf("Converted %d samples for playback (with TPDF dithering)\n", sampleCount);

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = rec->channels;
    wfx.nSamplesPerSec = rec->sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    printf("Attempting to open with format: channels=%d, sample rate=%lu, bits per sample=%d\n",
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

    int selCount = 0;
    int *indices = GetSelectedRecordingIndices(hwnd, &selCount);

    if (selCount == 0 || !indices) {
        MessageBox(hwnd, "No recordings selected", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    StopAudio();

    int savedCount = 0;
    int failedCount = 0;

    for (int i = 0; i < selCount; i++) {
        AudioRecording *rec = RecordingList_Get(&recordings, indices[i]);
        if (!rec || !rec->buffer || rec->size == 0) {
            failedCount++;
            continue;
        }

        // Generate filename with timestamp
        char filename[128];
        snprintf(filename, sizeof(filename), "recording_%02d%02d%02d_%02d%02d%02d.wav",
                 rec->timestamp.wYear % 100, rec->timestamp.wMonth, rec->timestamp.wDay,
                 rec->timestamp.wHour, rec->timestamp.wMinute, rec->timestamp.wSecond);

        float bufferPeak = CalculateBufferPeak(rec->buffer, rec->size);
        printf("Saving %s - peak: %.4f (%.1f dB)\n", filename, bufferPeak,
               bufferPeak > 0 ? 20.0f * log10f(bufferPeak) : -96.0f);

        FILE *file = fopen(filename, "wb");
        if (!file) {
            printf("Failed to open %s for writing\n", filename);
            failedCount++;
            continue;
        }

        WriteWavHeaderFloat(file, rec->sampleRate, rec->channels, rec->size);
        size_t written = fwrite(rec->buffer, 1, rec->size, file);
        fclose(file);

        if (written != rec->size) {
            printf("Failed to write all data to %s\n", filename);
            failedCount++;
        } else {
            printf("Saved: %s\n", filename);
            savedCount++;
        }
    }

    free(indices);

    // Show summary
    char msg[256];
    if (failedCount == 0) {
        snprintf(msg, sizeof(msg), "Saved %d recording%s (32-bit float)",
                 savedCount, savedCount == 1 ? "" : "s");
        MessageBox(hwnd, msg, "Success", MB_OK | MB_ICONINFORMATION);
    } else {
        snprintf(msg, sizeof(msg), "Saved %d, failed %d", savedCount, failedCount);
        MessageBox(hwnd, msg, "Partial Success", MB_OK | MB_ICONWARNING);
    }
}

void DeleteRecording(HWND hwnd)
{
    printf("DeleteRecording called\n");

    int selCount = 0;
    int *indices = GetSelectedRecordingIndices(hwnd, &selCount);

    if (selCount == 0 || !indices) {
        MessageBox(hwnd, "No recordings selected", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    StopAudio();

    // Delete in reverse order to avoid index shifting issues
    int deletedCount = 0;
    for (int i = selCount - 1; i >= 0; i--) {
        if (RecordingList_Remove(&recordings, indices[i])) {
            deletedCount++;
            printf("Deleted recording at index %d\n", indices[i]);
        }
    }

    free(indices);

    printf("Deleted %d recording(s)\n", deletedCount);
    RefreshRecordingList(hwnd, &recordings);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    printf("Application started\n");

    srand((unsigned int)time(NULL));

    RecordingList_Init(&recordings);

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
                UpdateButtonStates(hwnd, FALSE, TRUE);
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
        else if (msg.message == WM_USER + 5) // Delete recording
        {
            printf("Received Delete Recording message\n");
            DeleteRecording(hwnd);
        }
        else if (msg.message == WM_USER + 6) // Selection changed in listbox
        {
            int selCount = GetSelectedRecordingCount(hwnd);
            printf("Selection changed, %d item(s) selected\n", selCount);
            UpdateButtonStates(hwnd, selCount > 0, isRecording);
        }
        else if (msg.message == WM_RECORDING_ADDED) // Recording finished, refresh list
        {
            printf("Recording added, refreshing list\n");
            RefreshRecordingList(hwnd, &recordings);
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
    RecordingList_Free(&recordings);

    if (playbackBuffer) {
        free(playbackBuffer);
        playbackBuffer = NULL;
        printf("Freed playback buffer\n");
    }

    printf("Application exiting\n");
    return 0;
}
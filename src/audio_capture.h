// audio_capture.h
#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

typedef struct {
    IMMDeviceEnumerator *pEnumerator;
    IMMDevice *pDevice;
    IAudioClient *pAudioClient;
    IAudioCaptureClient *pCaptureClient;
    WAVEFORMATEX *pwfx;
    FILE *file;
    UINT32 bufferFrameCount;
    UINT32 bytesPerSample;
    UINT32 blockAlign;
    UINT32 captureBufferSize;
    BYTE *captureBuffer;
    DWORD dataLength;
} AudioCaptureContext;

HRESULT InitializeAudioCapture(AudioCaptureContext *ctx);
void CleanupAudioCapture(AudioCaptureContext *ctx);
HRESULT StartAudioCapture(AudioCaptureContext *ctx);
HRESULT CaptureAudioData(AudioCaptureContext *ctx);

#endif // AUDIO_CAPTURE_H

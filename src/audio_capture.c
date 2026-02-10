// audio_capture.c
#include <initguid.h>  
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

#include "audio_capture.h"

HRESULT InitializeAudioCapture(AudioCaptureContext *ctx) {
    HRESULT hr;

    // Initialize COM library
    hr = CoInitialize(NULL);
    if (FAILED(hr)) return hr;

    // Get the device enumerator
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&ctx->pEnumerator);
    if (FAILED(hr)) return hr;

    // Get the default audio render device
    hr = ctx->pEnumerator->lpVtbl->GetDefaultAudioEndpoint(ctx->pEnumerator, eRender, eConsole, &ctx->pDevice);
    if (FAILED(hr)) return hr;

    // Activate the IAudioClient interface
    hr = ctx->pDevice->lpVtbl->Activate(ctx->pDevice, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&ctx->pAudioClient);
    if (FAILED(hr)) return hr;

    // Get the mix format
    hr = ctx->pAudioClient->lpVtbl->GetMixFormat(ctx->pAudioClient, &ctx->pwfx);
    if (FAILED(hr)) return hr;

    // Debugging output: print sample rate and bit depth
    printf("Sample Rate: %lu\n", ctx->pwfx->nSamplesPerSec);
    printf("Channels: %d\n", ctx->pwfx->nChannels);
    printf("Bits per Sample: %d\n", ctx->pwfx->wBitsPerSample);

    // Initialize the audio client in loopback mode
    hr = ctx->pAudioClient->lpVtbl->Initialize(ctx->pAudioClient,
                                               AUDCLNT_SHAREMODE_SHARED,
                                               AUDCLNT_STREAMFLAGS_LOOPBACK,
                                               0, 0, ctx->pwfx, NULL);
    if (FAILED(hr)) return hr;

    // Get the capture client
    hr = ctx->pAudioClient->lpVtbl->GetService(ctx->pAudioClient, &IID_IAudioCaptureClient, (void**)&ctx->pCaptureClient);
    if (FAILED(hr)) return hr;

    // Calculate buffer sizes
    hr = ctx->pAudioClient->lpVtbl->GetBufferSize(ctx->pAudioClient, &ctx->bufferFrameCount);
    if (FAILED(hr)) return hr;

    ctx->bytesPerSample = ctx->pwfx->wBitsPerSample / 8;
    ctx->blockAlign = ctx->pwfx->nBlockAlign;
    ctx->captureBufferSize = ctx->bufferFrameCount * ctx->blockAlign;

    // Debugging output: print buffer size
    printf("Buffer Frame Count: %u\n", ctx->bufferFrameCount);    
    printf("Buffer Size: %d bytes\n", ctx->captureBufferSize);

    ctx->captureBuffer = (BYTE *)malloc(ctx->captureBufferSize);
    ctx->dataLength = 0;

    return S_OK;
}

void CleanupAudioCapture(AudioCaptureContext *ctx) {
    if (ctx->pCaptureClient) ctx->pCaptureClient->lpVtbl->Release(ctx->pCaptureClient);
    if (ctx->pAudioClient) ctx->pAudioClient->lpVtbl->Release(ctx->pAudioClient);
    if (ctx->pDevice) ctx->pDevice->lpVtbl->Release(ctx->pDevice);
    if (ctx->pEnumerator) ctx->pEnumerator->lpVtbl->Release(ctx->pEnumerator);
    if (ctx->pwfx) CoTaskMemFree(ctx->pwfx);
    if (ctx->captureBuffer) free(ctx->captureBuffer);
    if (ctx->file) fclose(ctx->file);
    CoUninitialize();
}

HRESULT StartAudioCapture(AudioCaptureContext *ctx) {
    return ctx->pAudioClient->lpVtbl->Start(ctx->pAudioClient);
}

// Note: The capture loop is implemented in RecordingThread() in main.c,
// which handles ring-buffered analysis alongside raw audio storage.
// StartAudioCapture() starts the WASAPI stream; the caller is responsible
// for calling GetBuffer/ReleaseBuffer in its own loop.
// audio_capture.c
#include <initguid.h>  
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

#include "audio_capture.h"
#include "audio_save.h"

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

HRESULT CaptureAudioData(AudioCaptureContext *ctx) {
    HRESULT hr;
    UINT32 packetLength = 0;
    BYTE *pData;
    DWORD flags;
    int capturing = 1;

    while (capturing) {
        // Sleep for a while
        Sleep(10);

        // Get the available data size
        hr = ctx->pCaptureClient->lpVtbl->GetNextPacketSize(ctx->pCaptureClient, &packetLength);
        if (FAILED(hr)) break;

        while (packetLength != 0) {
            // Get the captured data
            hr = ctx->pCaptureClient->lpVtbl->GetBuffer(ctx->pCaptureClient, &pData, &packetLength, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            UINT32 bytesToWrite = packetLength * ctx->blockAlign;

            // Write PCM data to WAV file
            fwrite(pData, bytesToWrite, 1, ctx->file);
            ctx->dataLength += bytesToWrite;

            // Release the buffer
            hr = ctx->pCaptureClient->lpVtbl->ReleaseBuffer(ctx->pCaptureClient, packetLength);
            if (FAILED(hr)) break;

            // Get the next packet size
            hr = ctx->pCaptureClient->lpVtbl->GetNextPacketSize(ctx->pCaptureClient, &packetLength);
            if (FAILED(hr)) break;
        }

        // Check for user interrupt (Ctrl+C)
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
            if (GetAsyncKeyState('C') & 0x8000) {
                capturing = 0;
            }
        }
    }

    // Stop recording
    ctx->pAudioClient->lpVtbl->Stop(ctx->pAudioClient);

    return hr;
}

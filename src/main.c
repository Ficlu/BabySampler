#include <stdio.h>
#include <signal.h>
#include "audio_capture.h"
#include "audio_save.h"

volatile int keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

int main() {
    HRESULT hr;
    WAVEFORMATEX *pwfx = NULL;
    AudioCaptureContext ctx = { 0 };

    // Initialize audio capture
    hr = InitializeAudioCapture(&ctx);
    if (FAILED(hr)) {
        printf("Failed to initialize audio capture: 0x%08lx\n", hr);  // Changed to %08lx
        return -1;
    }
    pwfx = ctx.pwfx;

    // Open output WAV file
    ctx.file = fopen("output.wav", "wb");
    if (!ctx.file) {
        printf("Failed to open output.wav for writing.\n");
        CleanupAudioCapture(&ctx);
        return -1;
    }
    WriteWavHeader(ctx.file, pwfx);

    printf("Recording... Press Ctrl+C to stop.\n");

    // Set up Ctrl+C handler
    signal(SIGINT, intHandler);

    // Start capturing and processing data
    hr = StartAudioCapture(&ctx);
    if (FAILED(hr)) {
        printf("Failed to start audio capture: 0x%08lx\n", hr);  // Changed to %08lx
        CleanupAudioCapture(&ctx);
        return -1;
    }

    // Main capture loop
    hr = CaptureAudioData(&ctx);
    if (FAILED(hr)) {
        printf("Error during audio capture: 0x%08lx\n", hr);  // Changed to %08lx
    }

    // Finalize output file
    FinalizeWavFile(ctx.file, ctx.dataLength);

    // Clean up
    CleanupAudioCapture(&ctx);

    printf("Recording stopped.\n");
    return 0;
}

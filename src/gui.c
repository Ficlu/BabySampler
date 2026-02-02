// gui.c
#include "gui.h"
#include <stdio.h>
#include <commctrl.h>
#include <math.h>

#define WINDOW_CLASS_NAME "AudioSamplerClass"

extern BOOL isPlaying;

HWND hStatus, hPlayButton, hSaveButton, hPeakMeter, hPeakText;

// Convert linear amplitude (0.0 to 1.0+) to dB
// Returns -infinity for 0, 0 dB for 1.0, positive for >1.0
static float LinearToDb(float linear)
{
    if (linear <= 0.0f) return -96.0f;  // Floor at -96 dB
    return 20.0f * log10f(linear);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        CreateGUIControls(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_START_BUTTON:
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;

        case ID_STOP_BUTTON:
            PostMessage(hwnd, WM_USER + 2, 0, 0);
            return 0;

        case ID_PLAY_BUTTON:
            PostMessage(hwnd, WM_USER + 3, 0, 0);
            return 0;

        case ID_SAVE_BUTTON:
            PostMessage(hwnd, WM_USER + 4, 0, 0);
            return 0;
        }
        break;

    case WM_UPDATE_PEAK:
        {
            // wParam contains peak value as integer (peak * 10000)
            float peakLinear = (float)wParam / 10000.0f;
            UpdatePeakMeter(peakLinear);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void CreateGUIControls(HWND hwnd)
{
    // Initialize common controls for progress bar
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    // Buttons
    CreateWindow("BUTTON", "Start Recording", WS_VISIBLE | WS_CHILD, 10, 10, 150, 30, hwnd, (HMENU)ID_START_BUTTON, NULL, NULL);
    CreateWindow("BUTTON", "Stop Recording", WS_VISIBLE | WS_CHILD, 170, 10, 150, 30, hwnd, (HMENU)ID_STOP_BUTTON, NULL, NULL);
    hPlayButton = CreateWindow("BUTTON", "Play", WS_VISIBLE | WS_CHILD, 10, 50, 150, 30, hwnd, (HMENU)ID_PLAY_BUTTON, NULL, NULL);
    hSaveButton = CreateWindow("BUTTON", "Save", WS_VISIBLE | WS_CHILD, 170, 50, 150, 30, hwnd, (HMENU)ID_SAVE_BUTTON, NULL, NULL);
    
    // Status text
    hStatus = CreateWindow("STATIC", "Not Recording", WS_VISIBLE | WS_CHILD, 10, 90, 310, 20, hwnd, NULL, NULL, NULL);

    // Peak meter label
    CreateWindow("STATIC", "Peak:", WS_VISIBLE | WS_CHILD, 10, 120, 40, 20, hwnd, NULL, NULL, NULL);

    // Peak meter progress bar
    // Range: 0 to 96 (representing -96 dB to 0 dB)
    // Values above 0 dB will peg at max
    hPeakMeter = CreateWindowEx(
        0,
        PROGRESS_CLASS,
        NULL,
        WS_VISIBLE | WS_CHILD,
        50, 120, 200, 20,
        hwnd,
        (HMENU)ID_PEAK_METER,
        NULL,
        NULL
    );
    SendMessage(hPeakMeter, PBM_SETRANGE, 0, MAKELPARAM(0, 96));
    SendMessage(hPeakMeter, PBM_SETPOS, 0, 0);

    // Peak text (shows dB value)
    hPeakText = CreateWindow("STATIC", "-∞ dB", WS_VISIBLE | WS_CHILD, 260, 120, 70, 20, hwnd, (HMENU)ID_PEAK_TEXT, NULL, NULL);

    EnableWindow(hPlayButton, FALSE);
    EnableWindow(hSaveButton, FALSE);
}

void UpdateRecordingStatus(HWND hwnd, BOOL isRecording)
{
    if (isRecording)
    {
        SetWindowText(hStatus, "Recording...");
        EnableWindow(hPlayButton, FALSE);
        EnableWindow(hSaveButton, FALSE);
        ResetPeakMeter();
    }
    else
    {
        SetWindowText(hStatus, "Not Recording");
        EnableWindow(hPlayButton, TRUE);
        EnableWindow(hSaveButton, TRUE);
    }
}

void UpdatePlayStatus(BOOL isPlaying)
{
    SetWindowText(hPlayButton, isPlaying ? "Stop" : "Play");
}

void UpdatePeakMeter(float peakLinear)
{
    float peakDb = LinearToDb(peakLinear);
    
    // Convert dB to progress bar position (0 = -96dB, 96 = 0dB)
    int meterPos = (int)(peakDb + 96.0f);
    if (meterPos < 0) meterPos = 0;
    if (meterPos > 96) meterPos = 96;
    
    SendMessage(hPeakMeter, PBM_SETPOS, meterPos, 0);
    
    // Update text
    char peakStr[32];
    if (peakLinear <= 0.0f) {
        snprintf(peakStr, sizeof(peakStr), "-inf dB");
    } else if (peakDb >= 0.0f) {
        // Clipping! Show in red-ish format
        snprintf(peakStr, sizeof(peakStr), "+%.1f dB!", peakDb);
    } else {
        snprintf(peakStr, sizeof(peakStr), "%.1f dB", peakDb);
    }
    SetWindowText(hPeakText, peakStr);
}

void ResetPeakMeter()
{
    SendMessage(hPeakMeter, PBM_SETPOS, 0, 0);
    SetWindowText(hPeakText, "-inf dB");
}

HWND InitializeGUI(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        WINDOW_CLASS_NAME,
        "Audio Sampler",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 350, 200,  // Made taller to fit meter
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (hwnd == NULL) {
        return NULL;
    }

    ShowWindow(hwnd, nCmdShow);

    return hwnd;
}
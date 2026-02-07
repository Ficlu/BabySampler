// gui.c
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <commctrl.h>
#include <math.h>

#define WINDOW_CLASS_NAME "AudioSamplerClass"

extern BOOL isPlaying;

// Control handles
HWND hStatus, hPlayButton, hSaveButton, hDeleteButton;
HWND hPeakMeter, hPeakText;
HWND hRecordingList, hMemoryText;

// Convert linear amplitude (0.0 to 1.0+) to dB
static float LinearToDb(float linear)
{
    if (linear <= 0.0f) return -96.0f;
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

        case ID_DELETE_BUTTON:
            PostMessage(hwnd, WM_USER + 5, 0, 0);
            return 0;

        case ID_RECORDING_LIST:
            // Handle selection change
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                PostMessage(hwnd, WM_USER + 6, 0, 0);  // Selection changed
            }
            return 0;
        }
        break;

    case WM_UPDATE_PEAK:
        {
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
    // Initialize common controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    int y = 10;

    // Record buttons row
    CreateWindow("BUTTON", "Start Recording", WS_VISIBLE | WS_CHILD, 
                 10, y, 145, 30, hwnd, (HMENU)ID_START_BUTTON, NULL, NULL);
    CreateWindow("BUTTON", "Stop Recording", WS_VISIBLE | WS_CHILD, 
                 165, y, 145, 30, hwnd, (HMENU)ID_STOP_BUTTON, NULL, NULL);
    y += 40;

    // Status text
    hStatus = CreateWindow("STATIC", "Not Recording", WS_VISIBLE | WS_CHILD, 
                           10, y, 300, 20, hwnd, NULL, NULL, NULL);
    y += 25;

    // Peak meter row
    CreateWindow("STATIC", "Peak:", WS_VISIBLE | WS_CHILD, 
                 10, y, 40, 20, hwnd, NULL, NULL, NULL);
    
    hPeakMeter = CreateWindowEx(0, PROGRESS_CLASS, NULL,
                                WS_VISIBLE | WS_CHILD,
                                55, y, 195, 20, hwnd,
                                (HMENU)ID_PEAK_METER, NULL, NULL);
    SendMessage(hPeakMeter, PBM_SETRANGE, 0, MAKELPARAM(0, 96));
    SendMessage(hPeakMeter, PBM_SETPOS, 0, 0);

    hPeakText = CreateWindow("STATIC", "-inf dB", WS_VISIBLE | WS_CHILD, 
                             255, y, 60, 20, hwnd, (HMENU)ID_PEAK_TEXT, NULL, NULL);
    y += 30;

    // Recordings label
    CreateWindow("STATIC", "Recordings:", WS_VISIBLE | WS_CHILD, 
                 10, y, 80, 20, hwnd, NULL, NULL, NULL);
    y += 22;

    // Recording list (extended selection for Ctrl+click, Shift+click)
    hRecordingList = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                                    WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL,
                                    10, y, 360, 100, hwnd,
                                    (HMENU)ID_RECORDING_LIST, NULL, NULL);
    y += 108;

    // Action buttons row
    hPlayButton = CreateWindow("BUTTON", "Play", WS_VISIBLE | WS_CHILD, 
                               10, y, 95, 30, hwnd, (HMENU)ID_PLAY_BUTTON, NULL, NULL);
    hSaveButton = CreateWindow("BUTTON", "Save", WS_VISIBLE | WS_CHILD, 
                               113, y, 95, 30, hwnd, (HMENU)ID_SAVE_BUTTON, NULL, NULL);
    hDeleteButton = CreateWindow("BUTTON", "Delete", WS_VISIBLE | WS_CHILD, 
                                 216, y, 95, 30, hwnd, (HMENU)ID_DELETE_BUTTON, NULL, NULL);
    y += 40;

    // Memory display
    hMemoryText = CreateWindow("STATIC", "Memory: 0 MB", WS_VISIBLE | WS_CHILD, 
                               10, y, 300, 20, hwnd, (HMENU)ID_MEMORY_TEXT, NULL, NULL);

    // Initial button states
    EnableWindow(hPlayButton, FALSE);
    EnableWindow(hSaveButton, FALSE);
    EnableWindow(hDeleteButton, FALSE);
}

void UpdateRecordingStatus(HWND hwnd, BOOL isRecording)
{
    if (isRecording)
    {
        SetWindowText(hStatus, "Recording...");
        ResetPeakMeter();
    }
    else
    {
        SetWindowText(hStatus, "Not Recording");
    }
}

void UpdatePlayStatus(BOOL playing)
{
    SetWindowText(hPlayButton, playing ? "Stop" : "Play");
}

void UpdatePeakMeter(float peakLinear)
{
    float peakDb = LinearToDb(peakLinear);
    
    int meterPos = (int)(peakDb + 96.0f);
    if (meterPos < 0) meterPos = 0;
    if (meterPos > 96) meterPos = 96;
    
    SendMessage(hPeakMeter, PBM_SETPOS, meterPos, 0);
    
    char peakStr[32];
    if (peakLinear <= 0.0f) {
        snprintf(peakStr, sizeof(peakStr), "-inf dB");
    } else if (peakDb >= 0.0f) {
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

void RefreshRecordingList(HWND hwnd, RecordingList *list)
{
    // Clear the listbox
    SendMessage(hRecordingList, LB_RESETCONTENT, 0, 0);

    // Add all recordings
    for (int i = 0; i < list->count; i++) {
        char entry[80];
        RecordingList_FormatEntry(&list->items[i], i, entry, sizeof(entry));
        SendMessage(hRecordingList, LB_ADDSTRING, 0, (LPARAM)entry);
    }

    // Restore selection (for extended sel, use LB_SETSEL)
    if (list->selected >= 0 && list->selected < list->count) {
        SendMessage(hRecordingList, LB_SETSEL, TRUE, list->selected);
    }

    // Update button states
    int selCount = GetSelectedRecordingCount(hwnd);
    UpdateButtonStates(hwnd, selCount > 0, FALSE);

    // Update memory display
    UpdateMemoryDisplay(hwnd, RecordingList_GetTotalMemory(list));
}

void UpdateMemoryDisplay(HWND hwnd, DWORD totalBytes)
{
    char memStr[64];
    float megabytes = (float)totalBytes / (1024.0f * 1024.0f);
    snprintf(memStr, sizeof(memStr), "Memory: %.1f MB", megabytes);
    SetWindowText(hMemoryText, memStr);
}

void UpdateButtonStates(HWND hwnd, BOOL hasSelection, BOOL isRecording)
{
    if (isRecording) {
        EnableWindow(hPlayButton, FALSE);
        EnableWindow(hSaveButton, FALSE);
        EnableWindow(hDeleteButton, FALSE);
    } else {
        EnableWindow(hPlayButton, hasSelection);
        EnableWindow(hSaveButton, hasSelection);
        EnableWindow(hDeleteButton, hasSelection);
    }
}

int GetSelectedRecordingIndex(HWND hwnd)
{
    // For multi-select, get the first selected item (for playback)
    int count = (int)SendMessage(hRecordingList, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) return -1;
    
    int index = -1;
    SendMessage(hRecordingList, LB_GETSELITEMS, 1, (LPARAM)&index);
    return index;
}

int GetSelectedRecordingCount(HWND hwnd)
{
    LRESULT count = SendMessage(hRecordingList, LB_GETSELCOUNT, 0, 0);
    return (count == LB_ERR) ? 0 : (int)count;
}

int* GetSelectedRecordingIndices(HWND hwnd, int *outCount)
{
    int count = GetSelectedRecordingCount(hwnd);
    *outCount = count;
    
    if (count <= 0) return NULL;
    
    int *indices = (int*)malloc(count * sizeof(int));
    if (!indices) {
        *outCount = 0;
        return NULL;
    }
    
    SendMessage(hRecordingList, LB_GETSELITEMS, count, (LPARAM)indices);
    return indices;
}

void SetRecordingSelection(HWND hwnd, int index)
{
    // Clear all selections first
    SendMessage(hRecordingList, LB_SETSEL, FALSE, -1);
    // Select the specified index
    if (index >= 0) {
        SendMessage(hRecordingList, LB_SETSEL, TRUE, index);
    }
}

HWND InitializeGUI(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    // Taller and wider window to fit the recording list with scale info
    HWND hwnd = CreateWindowEx(
        0,
        WINDOW_CLASS_NAME,
        "Audio Sampler",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 360,
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
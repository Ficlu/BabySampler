// gui.c
#include "gui.h"
#include <stdio.h>

#define WINDOW_CLASS_NAME "AudioSamplerClass"

extern BOOL isPlaying;

HWND hStatus, hPlayButton, hSaveButton;

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

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void CreateGUIControls(HWND hwnd)
{
    CreateWindow("BUTTON", "Start Recording", WS_VISIBLE | WS_CHILD, 10, 10, 150, 30, hwnd, (HMENU)ID_START_BUTTON, NULL, NULL);
    CreateWindow("BUTTON", "Stop Recording", WS_VISIBLE | WS_CHILD, 170, 10, 150, 30, hwnd, (HMENU)ID_STOP_BUTTON, NULL, NULL);
    hPlayButton = CreateWindow("BUTTON", "Play", WS_VISIBLE | WS_CHILD, 10, 50, 150, 30, hwnd, (HMENU)ID_PLAY_BUTTON, NULL, NULL);
    hSaveButton = CreateWindow("BUTTON", "Save", WS_VISIBLE | WS_CHILD, 170, 50, 150, 30, hwnd, (HMENU)ID_SAVE_BUTTON, NULL, NULL);
    hStatus = CreateWindow("STATIC", "Not Recording", WS_VISIBLE | WS_CHILD, 10, 90, 310, 20, hwnd, NULL, NULL, NULL);

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

HWND InitializeGUI(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS_NAME;

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        WINDOW_CLASS_NAME,
        "Audio Sampler",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 350, 160,
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
// gui.h
#ifndef GUI_H
#define GUI_H

#include <windows.h>
#include "recording_list.h"

#define ID_START_BUTTON   1001
#define ID_STOP_BUTTON    1002
#define ID_PLAY_BUTTON    1003
#define ID_SAVE_BUTTON    1004
#define ID_PEAK_METER     1005
#define ID_PEAK_TEXT      1006
#define ID_DELETE_BUTTON  1007
#define ID_RECORDING_LIST 1008
#define ID_MEMORY_TEXT    1009

// Custom messages
#define WM_UPDATE_PEAK       (WM_USER + 100)
#define WM_RECORDING_ADDED   (WM_USER + 101)
#define WM_RECORDING_REMOVED (WM_USER + 102)
#define WM_UPDATE_MEMORY     (WM_USER + 103)

extern BOOL isPlaying;

HWND InitializeGUI(HINSTANCE hInstance, int nCmdShow);
void CreateGUIControls(HWND hwnd);
void UpdateRecordingStatus(HWND hwnd, BOOL isRecording);
void UpdatePlayStatus(BOOL isPlaying);
void UpdatePeakMeter(float peakLinear);
void ResetPeakMeter();

// Recording list management
void RefreshRecordingList(HWND hwnd, RecordingList *list);
void UpdateMemoryDisplay(HWND hwnd, DWORD totalBytes);
void UpdateButtonStates(HWND hwnd, BOOL hasSelection, BOOL isRecording);
int GetSelectedRecordingIndex(HWND hwnd);
int GetSelectedRecordingCount(HWND hwnd);
int* GetSelectedRecordingIndices(HWND hwnd, int *outCount);  // Caller must free() result
void SetRecordingSelection(HWND hwnd, int index);

#endif // GUI_H
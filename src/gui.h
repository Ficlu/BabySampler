// gui.h
#ifndef GUI_H
#define GUI_H

#include <windows.h>

#define ID_START_BUTTON 1001
#define ID_STOP_BUTTON 1002
#define ID_PLAY_BUTTON 1003
#define ID_SAVE_BUTTON 1004
#define ID_PEAK_METER 1005
#define ID_PEAK_TEXT 1006

// Custom message for peak meter updates
#define WM_UPDATE_PEAK (WM_USER + 100)

extern BOOL isPlaying;

HWND InitializeGUI(HINSTANCE hInstance, int nCmdShow);
void CreateGUIControls(HWND hwnd);
void UpdateRecordingStatus(HWND hwnd, BOOL isRecording);
void UpdatePlayStatus(BOOL isPlaying);
void UpdatePeakMeter(float peakLinear);
void ResetPeakMeter();

#endif // GUI_H
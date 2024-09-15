// gui.h
#ifndef GUI_H
#define GUI_H

#include <windows.h>

#define ID_START_BUTTON 1001
#define ID_STOP_BUTTON 1002
#define ID_PLAY_BUTTON 1003
#define ID_SAVE_BUTTON 1004

extern BOOL isPlaying;

HWND InitializeGUI(HINSTANCE hInstance, int nCmdShow);
void CreateGUIControls(HWND hwnd);
void UpdateRecordingStatus(HWND hwnd, BOOL isRecording);
void UpdatePlayStatus(BOOL isPlaying);

#endif // GUI_H
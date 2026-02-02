// recording_list.h
#ifndef RECORDING_LIST_H
#define RECORDING_LIST_H

#include <windows.h>

typedef struct {
    BYTE *buffer;
    DWORD size;           // bytes captured
    DWORD sampleRate;
    WORD channels;
    SYSTEMTIME timestamp; // when it was recorded
    float peakLevel;      // for display
    float duration;       // in seconds
} AudioRecording;

typedef struct {
    AudioRecording *items;
    int count;
    int capacity;
    int selected;         // currently selected index, -1 if none
} RecordingList;

// Initialize a recording list
void RecordingList_Init(RecordingList *list);

// Free all recordings and the list itself
void RecordingList_Free(RecordingList *list);

// Add a new recording (takes ownership of buffer - don't free it after calling)
// Returns the index of the new recording, or -1 on failure
int RecordingList_Add(RecordingList *list, BYTE *buffer, DWORD size,
                      DWORD sampleRate, WORD channels, float peakLevel);

// Remove a recording at index and free its buffer
// Returns TRUE on success, FALSE if index is invalid
BOOL RecordingList_Remove(RecordingList *list, int index);

// Get a recording by index (returns NULL if invalid)
AudioRecording* RecordingList_Get(RecordingList *list, int index);

// Get currently selected recording (returns NULL if none selected)
AudioRecording* RecordingList_GetSelected(RecordingList *list);

// Get total memory usage in bytes
DWORD RecordingList_GetTotalMemory(RecordingList *list);

// Format a recording entry for display in listbox
// Buffer should be at least 64 chars
void RecordingList_FormatEntry(AudioRecording *rec, int index, char *buffer, int bufferSize);

#endif // RECORDING_LIST_H
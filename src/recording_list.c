// recording_list.c
#include "recording_list.h"
#include "scale_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define INITIAL_CAPACITY 8

void RecordingList_Init(RecordingList *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->selected = -1;
}

void RecordingList_Free(RecordingList *list)
{
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            if (list->items[i].buffer) {
                free(list->items[i].buffer);
                list->items[i].buffer = NULL;
            }
        }
        free(list->items);
        list->items = NULL;
    }
    list->count = 0;
    list->capacity = 0;
    list->selected = -1;
}

int RecordingList_Add(RecordingList *list, BYTE *buffer, DWORD size,
                      DWORD sampleRate, WORD channels, float peakLevel,
                      const ScaleResult *scale)
{
    // Grow array if needed
    if (list->count >= list->capacity) {
        int newCapacity = list->capacity == 0 ? INITIAL_CAPACITY : list->capacity * 2;
        AudioRecording *newItems = (AudioRecording *)realloc(list->items, 
                                                              newCapacity * sizeof(AudioRecording));
        if (!newItems) {
            return -1;
        }
        list->items = newItems;
        list->capacity = newCapacity;
    }

    // Calculate duration
    // Size is in bytes, each sample is 4 bytes (32-bit float), divided by channels
    float duration = 0.0f;
    if (sampleRate > 0 && channels > 0) {
        DWORD sampleCount = size / (sizeof(float) * channels);
        duration = (float)sampleCount / (float)sampleRate;
    }

    // Fill in the new entry
    int index = list->count;
    AudioRecording *rec = &list->items[index];
    rec->buffer = buffer;
    rec->size = size;
    rec->sampleRate = sampleRate;
    rec->channels = channels;
    rec->peakLevel = peakLevel;
    rec->duration = duration;
    GetLocalTime(&rec->timestamp);
    
    // Copy scale result
    if (scale) {
        rec->scale = *scale;
    } else {
        memset(&rec->scale, 0, sizeof(ScaleResult));
        rec->scale.rootNote = -1;
        rec->scale.scaleType = SCALE_UNKNOWN;
    }

    list->count++;

    // Auto-select the new recording
    list->selected = index;

    return index;
}

BOOL RecordingList_Remove(RecordingList *list, int index)
{
    if (index < 0 || index >= list->count) {
        return FALSE;
    }

    // Free the buffer
    if (list->items[index].buffer) {
        free(list->items[index].buffer);
        list->items[index].buffer = NULL;
    }

    // Shift remaining items down
    for (int i = index; i < list->count - 1; i++) {
        list->items[i] = list->items[i + 1];
    }
    list->count--;

    // Update selection
    if (list->count == 0) {
        list->selected = -1;
    } else if (list->selected >= list->count) {
        list->selected = list->count - 1;
    } else if (list->selected > index) {
        list->selected--;
    }

    return TRUE;
}

AudioRecording* RecordingList_Get(RecordingList *list, int index)
{
    if (index < 0 || index >= list->count) {
        return NULL;
    }
    return &list->items[index];
}

AudioRecording* RecordingList_GetSelected(RecordingList *list)
{
    return RecordingList_Get(list, list->selected);
}

DWORD RecordingList_GetTotalMemory(RecordingList *list)
{
    DWORD total = 0;
    for (int i = 0; i < list->count; i++) {
        total += list->items[i].size;
    }
    return total;
}

void RecordingList_FormatEntry(AudioRecording *rec, int index, char *buffer, int bufferSize)
{
    // Format: "1. 14:32:05 - 12.3s (-3.2 dB) [Am 87%]"
    float peakDb = rec->peakLevel > 0 ? 20.0f * log10f(rec->peakLevel) : -96.0f;
    
    char scaleName[16];
    ScaleResult_GetShortName(&rec->scale, scaleName, sizeof(scaleName));
    
    // Show confidence as percentage if we have a valid scale
    if (rec->scale.scaleType != SCALE_UNKNOWN && rec->scale.confidence > 0) {
        int confidencePct = (int)(rec->scale.confidence * 100.0f + 0.5f);
        snprintf(buffer, bufferSize, "%d. %02d:%02d:%02d - %.1fs (%.1f dB) [%s %d%%]",
                 index + 1,
                 rec->timestamp.wHour,
                 rec->timestamp.wMinute,
                 rec->timestamp.wSecond,
                 rec->duration,
                 peakDb,
                 scaleName,
                 confidencePct);
    } else {
        snprintf(buffer, bufferSize, "%d. %02d:%02d:%02d - %.1fs (%.1f dB) [%s]",
                 index + 1,
                 rec->timestamp.wHour,
                 rec->timestamp.wMinute,
                 rec->timestamp.wSecond,
                 rec->duration,
                 peakDb,
                 scaleName);
    }
}
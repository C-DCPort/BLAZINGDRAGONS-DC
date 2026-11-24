#pragma once
bool dcAudioStart();
void dcAudioStats(unsigned long *msRender, unsigned long *frames, int *voices);
unsigned long dcAudioMaxGap();
void dcAudioHwStats(unsigned *uploads, unsigned *fails, unsigned *kb);

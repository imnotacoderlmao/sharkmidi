#ifndef PLAYBACK_THREAD_H
#define PLAYBACK_THREAD_H
#include <stdint.h>
void StartPlayback(int singlethread);
void Playback_StopAndWait(void); // blocks until the playback loop has fully exited
extern int64_t playednotes, notespersec;
extern volatile int paused, stopping;
extern int64_t current_clock;
extern double tick, delta, tickscale, bpm, midifps;
void clock_pause(void);
void clock_skip(double ticks); // relative seek, applied by the playback thread
void UpdatePlaybackStats(void);
#endif

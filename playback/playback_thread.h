#ifndef PLAYBACK_THREAD_H
#define PLAYBACK_THREAD_H
#include <stdint.h>
#include <stdatomic.h>
void StartPlayback(int singlethread);
void Playback_StopAndWait(void); // blocks until the playback loop has fully exited
extern int64_t playednotes, notespersec;
extern volatile int stopping;
extern int64_t current_clock;
extern _Atomic double playback_next_due; // when the playback thread will next look at the clock, for the audio thread to sync its polling to
extern double tick, delta, tickscale, bpm, spin_margin;
void clock_pause(void);
void clock_skip(double ticks); // relative seek, applied by the playback thread
void UpdatePlaybackStats(void);
#endif

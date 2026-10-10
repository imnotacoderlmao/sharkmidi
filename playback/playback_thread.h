#include <stdint.h>
void StartPlayback(int singlethread);
extern int64_t playednotes, notespersec;
extern volatile int paused, stopping;
extern int64_t current_clock;
extern double tick, delta, tickscale, bpm, midifps;
void clock_pause(void);
void clock_skip(double skiptick, int skipto);
void UpdatePlaybackStats();
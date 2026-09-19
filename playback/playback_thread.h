#include <stdint.h>
void StartPlayback(int singlethread);
extern volatile int stopping;
extern int64_t playednotes, notespersec;
extern volatile int paused;
extern double tick, last, tickscale, bpm, midifps;
void clock_pause(void);
void clock_skip(double skiptick, int skipto);
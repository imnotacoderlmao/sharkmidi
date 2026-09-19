#include <stdint.h>
void StartPlayback(int singlethread);
extern volatile int stopping;
extern int32_t current_clock;
extern int64_t playednotes, notespersec;
extern volatile int paused;
extern volatile double tickscale, bpm;
void clock_pause(void);
void clock_skip(double skiptick, int skipto);
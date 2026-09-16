#include <stdint.h>
void StartPlayback(int32_t singlethread);
extern volatile int stopping;
extern int32_t current_clock;
extern volatile int paused;
extern volatile double tickscale;
void clock_pause(void);
void clock_skip(double skiptick, int skipto);
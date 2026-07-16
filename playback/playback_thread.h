void StartPlayback(int32_t singlethread);
extern int32_t stopping;
extern int32_t current_clock;
extern int32_t paused;
void clock_pause(void);
void clock_resume(void);
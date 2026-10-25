#ifndef SYNTH
#define SYNTH
#include "KDMAPI.h"
#include <stdatomic.h>
#include <stdint.h>

#define RINGBUFFER_SIZE 524288u
#define RINGBUFFER_MASK (RINGBUFFER_SIZE - 1u)

int Sound_Init(int singlethreaded);
void Sound_Close(void);
void AllNotesOFF(void);
void reinitsynths(void);
int audiothread_entry(void);                       // 1 if the audio thread runs, 0 if there is no memory or thread for it (then send directly)
void audiothread_join(void);
int Sound_Send(const uint8_t* p, const uint8_t* end); // 1 if events went to the audio thread, 0 if there was no room
void Sound_Flush(void);                               // a seek: what the audio thread has not sent yet is dropped
extern int issynthinitiated, singlethread, voicefetching;
#endif
#ifndef SYNTH
#define SYNTH
#include "KDMAPI.h"
#include "../misc/uint24.h"
#include <stdatomic.h>
#include <stdint.h>

#define RINGBUFFER_SIZE 2097152u
#define RINGBUFFER_MASK (RINGBUFFER_SIZE - 1u)

int Sound_Init(int singlethreaded);
void Sound_Close(void);
void AllNotesOFF(void);
void reinitsynths(void);
void audiothread_entry(void);
void audiothread_join(void);
extern int issynthinitiated, singlethread, voicefetching;
extern uint24_t* ringbuffer;
extern _Atomic uint32_t writeptr;
#endif

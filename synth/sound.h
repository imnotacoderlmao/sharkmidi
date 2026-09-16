#include "../misc/uint24.h"
#ifndef SYNTH
#define SYNTH

#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    extern int32_t (*PrepareLongData)(MIDIHDR* hdr, int32_t hdrsize);
    extern int32_t (*UnprepareLongData)(MIDIHDR* hdr, int32_t hdrsize);
    extern int32_t (*SendDirectLongData)(MIDIHDR* hdr, int32_t hdrsize);
#else
    extern int32_t (*SendDirectLongData)(uint8_t* message, int32_t messagesize);
#endif
int32_t Sound_Init(int32_t singlethread);
void AllNotesOFF(void);
extern void (*SendDirectData)(uint32_t message);
extern int issynthinitiated;
extern int32_t (*GetVoiceCount)(void);
extern uint24_t* ringbuffer;
extern int32_t voicefetching;
extern volatile uint32_t writeptr;
extern const uint32_t RINGBUFFER_SIZE;
extern const uint32_t RINGBUFFER_MASK;
#endif
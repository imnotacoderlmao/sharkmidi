#ifndef midistorage_hdr
#define midistorage_hdr
#include "../misc/uint24.h"
#include <stdint.h>
typedef struct
{
    int32_t tick;
    int32_t notecount;
    int64_t event_offset;
} TickGroup;

typedef struct 
{
    int32_t tick;
    uint24_t microsec;
} TempoEvent;

typedef struct 
{
    int32_t tick;
    int32_t size;
    uint8_t* message;
} SysExEvent;


extern TickGroup* timingArr;
extern uint24_t* eventArr;
extern SysExEvent* sysexArr;
extern TempoEvent* tempoArr;
extern uint64_t totalnotes;
extern uint32_t tempoCount;
extern uint32_t sysexCount;
extern uint32_t ppq;
extern int32_t maxTick;
#endif
#ifndef midistorage_hdr
#define midistorage_hdr
#include "../misc/uint24.h"
#include <stdint.h>
typedef struct
{
    int64_t tick;
    int64_t notecount;
    int64_t event_offset;
} TickGroup;

typedef struct {
    int64_t event_offset; // Starting index in eventArr
    uint32_t count;       // Number of events in this run
    uint8_t track;        // Track ID
} TrackGroup;

typedef struct {
    TrackGroup* data;
    int64_t count;
} TrackGroup_arr;

typedef struct 
{
    int64_t tick;
    int32_t microsec;
} TempoEvent;

typedef struct 
{
    int64_t tick;
    int32_t size;
    uint8_t* message;
} SysExEvent;


extern TickGroup* timingArr;
extern uint24_t* eventArr;
extern TrackGroup_arr trackGroups;
extern SysExEvent* sysexArr;
extern TempoEvent* tempoArr;
extern uint64_t totalnotes;
extern uint32_t tempoCount;
extern uint32_t sysexCount;
extern uint32_t ppq;
extern int64_t maxTick, activetickcount;
extern const char* filename;
extern int filename_len;
#endif
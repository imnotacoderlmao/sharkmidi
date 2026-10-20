#include "midistorage.h"
#include <stddef.h>
TickGroup* timingArr = NULL;
uint8_t* eventArr = NULL;
TrackGroupArr trackGroups = {0};
SysExEvent* sysexArr = NULL;
TempoEvent* tempoArr = NULL;
TimeSigEvent* timesigArr = NULL; 
uint64_t totalnotes = 0;
uint32_t ppq = 480;
uint32_t tempoCount = 0;
uint32_t sysexCount = 0;
uint32_t timesigCount = 0;  
int64_t maxTick = 0, activetickcount = 0;
const char* filename = "to load a midi, drag and drop one to the window";
int filename_len = 0;

// index of the last signature that starts at or before tick
static uint32_t ts_by_tick(int64_t tick)
{
    uint32_t lo = 0, hi = timesigCount;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        if (timesigArr[mid].tick <= tick) lo = mid + 1; else hi = mid;
    }
    return lo ? lo - 1 : 0;
}

// index of the last signature whose first measure is at or before `measure`
static uint32_t ts_by_measure(int64_t measure)
{
    uint32_t lo = 0, hi = timesigCount;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        if (timesigArr[mid].measure <= measure) lo = mid + 1; else hi = mid;
    }
    return lo ? lo - 1 : 0;
}

int64_t TimeSig_MeasureAt(int64_t tick)
{
    if (tick < 0) tick = 0;
    if (!timesigCount)
        return tick / ((int64_t)ppq * 4 > 0 ? (int64_t)ppq * 4 : 1);
    const TimeSigEvent* e = &timesigArr[ts_by_tick(tick)];
    return e->measure + (tick - e->tick) / e->measure_ticks;
}

int64_t TimeSig_MeasureStart(int64_t measure)
{
    if (measure < 0) measure = 0;
    if (!timesigCount)
        return measure * ((int64_t)ppq * 4 > 0 ? (int64_t)ppq * 4 : 1);
    const TimeSigEvent* e = &timesigArr[ts_by_measure(measure)];
    return e->tick + (measure - e->measure) * e->measure_ticks;
}

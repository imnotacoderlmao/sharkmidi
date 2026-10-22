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

// index of the last signature that starts at or before key, in ticks or, with by_measure, in measures
static uint32_t ts_find(int64_t key, int by_measure)
{
    uint32_t lo = 0, hi = timesigCount;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        int64_t at = by_measure ? timesigArr[mid].measure : timesigArr[mid].tick;
        if (at <= key) lo = mid + 1; else hi = mid;
    }
    return lo ? lo - 1 : 0;
}

int64_t TimeSig_MeasureAt(int64_t tick)
{
    if (tick < 0) tick = 0;
    if (!timesigCount)
        return tick / (ppq * 4);
    const TimeSigEvent* e = &timesigArr[ts_find(tick, 0)];
    return e->measure + (tick - e->tick) / e->measure_ticks;
}

int64_t TimeSig_MeasureStart(int64_t measure)
{
    if (measure < 0) measure = 0;
    if (!timesigCount)
        return measure * (ppq * 4);
    const TimeSigEvent* e = &timesigArr[ts_find(measure, 1)];
    return e->tick + (measure - e->measure) * e->measure_ticks;
}

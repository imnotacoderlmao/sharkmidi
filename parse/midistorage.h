#ifndef midistorage_hdr
#define midistorage_hdr
#include "../misc/uint24.h"
#include <stdint.h>
// eventArr is a byte stream, and every event_offset (timingArr, the track index) is a position in it, in bytes.
// the events of one track at one tick (a group) are stored as runs of events that share a status and a third
// byte, which in black midi is nearly all of them:
//     status      0x80 - 0xEF, as the event is stored (a note on with velocity 0 is a note off with velocity 64)
//     third byte  0x00 - 0x7F (0 for the events that only have one data byte)
//     notes       one byte per event, 0x00 - 0x7F, bit 7 set on the last one of the run
// a run never crosses a group, so any group, and so any tick, starts a fresh run
typedef struct
{
    int64_t tick;
    int64_t notecount;
    int64_t event_offset; // in bytes
} TickGroup;

// decodes the runs in [p, end) of eventArr: ev[0..2] is the event as the old 3 byte entries had it
#define EV_RUNS(p, end, ev, BODY)                                              \
    do {                                                                       \
        const uint8_t* ev_p_ = (p);                                            \
        const uint8_t* ev_end_ = (end);                                        \
        while (ev_p_ < ev_end_)                                                \
        {                                                                      \
            uint8_t ev[3];                                                     \
            ev[0] = ev_p_[0];                                                  \
            ev[2] = ev_p_[1];                                                  \
            ev_p_ += 2;                                                        \
            uint8_t ev_n_;                                                     \
            do {                                                               \
                ev_n_ = *ev_p_++;                                              \
                ev[1] = ev_n_ & 0x7F;                                          \
                BODY                                                           \
            } while (!(ev_n_ & 0x80));                                         \
        }                                                                      \
    } while (0)

// the track index: one run per (tick, track) group, in event order, covering eventArr back to back. a run
// is 4 bytes, count << 4 | track & 15. its start offset is not stored, it is the sum of the counts before it,
// so one anchor (the start offset of every TG_ANCHOR-th run) is kept for random access
#define TG_ANCHOR 64
#define TG_COUNT_MAX 0x0FFFFFFFu
typedef struct {
    uint32_t* runs;
    int64_t* anchors;   // anchors[i] = start offset of run i * TG_ANCHOR
    int64_t count;      // number of runs
} TrackGroup_arr;

static inline int64_t tg_run_count(uint32_t run) { return run >> 4; }
static inline uint8_t tg_run_track(uint32_t run) { return (uint8_t)((run & 15) << 4); }

// last run starting at or before targetOffset (0 when there is none), its start offset goes to *start
static inline int64_t tg_find(const TrackGroup_arr* tg, int64_t targetOffset, int64_t* start)
{
    int64_t anchors = (tg->count + TG_ANCHOR - 1) / TG_ANCHOR;
    int64_t low = 0, high = anchors - 1, a = 0;
    while (low <= high)
    {
        int64_t mid = low + (high - low) / 2;
        if (tg->anchors[mid] <= targetOffset) { a = mid; low = mid + 1; }
        else high = mid - 1;
    }
    int64_t i = a * TG_ANCHOR, off = tg->anchors[a];
    int64_t end = i + TG_ANCHOR < tg->count ? i + TG_ANCHOR : tg->count;
    while (i + 1 < end && off + tg_run_count(tg->runs[i]) <= targetOffset)
        off += tg_run_count(tg->runs[i++]);
    *start = off;
    return i;
}

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
extern uint8_t* eventArr;
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
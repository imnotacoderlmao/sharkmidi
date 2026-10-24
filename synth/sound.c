// also thank conmidi for existing
#include "sound.h"
#include "../playback/playback_thread.h"
#include "../playback/timer.h"
#include "../parse/midistorage.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

int issynthinitiated = 0, singlethread = 0, voicefetching = 0;
static pthread_t audio_thread;

// the events go from the playback thread to the audio thread as packets in a byte ring: a 4 byte length, then that many
// bytes of runs (see midistorage.h). a packet is whole runs, so the audio thread can decode it on its own. positions
// count bytes since playback started and only grow, position & RINGBUFFER_MASK is where it is in the ring. the playback
// thread owns ring_w and the audio thread ring_r, so a packet that is not read yet is never written over: what does
// not fit in the room between them is dropped
static uint8_t* ring = NULL;
static uint8_t* packet = NULL;        // the audio thread's copy of the packet it is sending, a packet is never bigger than the ring
static _Atomic uint64_t ring_w, ring_r;
static _Atomic uint64_t ring_flush;   // a seek: what is in the ring below this is stale
static uint64_t flushed;              // the playback thread's own copy of it

static inline void ring_put(uint64_t at, const void* src, size_t n)
{
    size_t i = at & RINGBUFFER_MASK, first = RINGBUFFER_SIZE - i;
    if (n <= first)
    {
        memcpy(ring + i, src, n);
        return;
    }
    memcpy(ring + i, src, first);
    memcpy(ring, (const uint8_t*)src + first, n - first);
}

static inline void ring_get(uint64_t at, void* dst, size_t n)
{
    size_t i = at & RINGBUFFER_MASK, first = RINGBUFFER_SIZE - i;
    if (n <= first)
    {
        memcpy(dst, ring + i, n);
        return;
    }
    memcpy(dst, ring + i, first);
    memcpy((uint8_t*)dst + first, ring, n - first);
}

// sends the runs in [p, end), one group of events. when they don't fit in the room the audio thread has left, the last
// ones that do are sent, and since a packet has to start on a run, a run that is cut gets its header (status and third
// byte) put back. a status is the only byte that follows the last note of the run before it, and that note and the
// status are the only two bytes in a row with bit 7 set, so the run a byte is in starts at the last such pair before it
int Sound_Send(const uint8_t* p, const uint8_t* end)
{
    uint64_t w = atomic_load_explicit(&ring_w, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&ring_r, memory_order_acquire);
    if (r < flushed) r = flushed; // the audio thread has not got to the seek yet, what it has left is stale
    uint64_t room = RINGBUFFER_SIZE - (w - r);
    if (p >= end || room < 4 + 3) // a packet is 4 bytes of length and a run of 3 at the least
        return 0;
    uint64_t cap = room - 4;
    const uint8_t* q = p;
    uint8_t head[2];
    uint32_t headn = 0;
    if ((uint64_t)(end - p) > cap)
    {
        q = end - (cap - 2); // room for a header, in case q is in the middle of a run
        const uint8_t* h = q;
        while (h > p && !(h[0] & h[-1] & 0x80)) h--;
        if (h != q)
        {
            if (q <= h + 1) q = h; // q is in the header of the run: take all of it
            else { head[0] = h[0]; head[1] = h[1]; headn = 2; }
        }
    }
    uint32_t len = headn + (uint32_t)(end - q);
    ring_put(w, &len, 4);
    if (headn)
        ring_put(w + 4, head, headn);
    ring_put(w + 4 + headn, q, end - q);
    atomic_store_explicit(&ring_w, w + 4 + len, memory_order_release);
    return 1;
}

void Sound_Flush(void)
{
    flushed = atomic_load_explicit(&ring_w, memory_order_relaxed);
    atomic_store_explicit(&ring_flush, flushed, memory_order_release);
}

// sends the next packet, 0 when there is none. r is where the audio thread is in the ring
static int send_packet(uint64_t* rp)
{
    uint64_t r = *rp;
    uint64_t flush = atomic_load_explicit(&ring_flush, memory_order_acquire);
    if (r < flush)
    {
        r = flush;
        atomic_store_explicit(&ring_r, r, memory_order_release);
    }
    uint64_t w = atomic_load_explicit(&ring_w, memory_order_acquire);
    uint32_t len = 0;
    int sent = r != w;
    if (sent)
    {
        ring_get(r, &len, 4);
        if ((uint64_t)len + 4 > w - r) // not a packet, drop everything that is there
        {
            r = w;
            len = 0;
        }
        else
        {
            ring_get(r + 4, packet, len);
            r += 4 + len;
        }
        atomic_store_explicit(&ring_r, r, memory_order_release); // the room is free again while this one is sent
    }
    *rp = r;
    if (sent)
        EV_RUNS(packet, packet + len, st, third, key,
            const uint32_t head = st | (third << 16);,
            KDMAPI_SendDirectData(head | (key << 8)););
    return sent;
}

// when the ring is empty the thread sleeps until the playback thread's next tick boundary (it writes to playback_next_due that moment),
// it wakes AUDIO_SPIN_US early and spins the rest, so events go out as they are written
static void* audiothread(void* args)
{
    (void)args;
    puts("audio thread intialized\n");
    os_timer_init(); // timer slack on Linux is per thread, and the Windows timer handle is per source file
    uint64_t r = 0;
    while (!stopping)
    {
        if (send_packet(&r))
            continue;
        double now = get_time();
        double due = atomic_load_explicit(&playback_next_due, memory_order_relaxed);
        if ((due + spin_margin) > now)
        {
            double wake = due - DEFAULT_SPIN_US * 1e-6;
            if (wake > now)
                os_sleep_until(wake);
            else
                os_cpu_relax();
        }
        else
            os_sleep_until(now + DEFAULT_IDLE_US * 1e-6);
    }
    return NULL;
}

void audiothread_entry(void)
{
    atomic_store(&ring_w, 0);
    atomic_store(&ring_r, 0);
    atomic_store(&ring_flush, 0);
    flushed = 0;
    pthread_create(&audio_thread, NULL, audiothread, NULL);
}

// must run before AllNotesOFF, otherwise the audio thread can still push events after it
void audiothread_join(void)
{
    pthread_join(audio_thread, NULL);
}

int Sound_Init(int singlethreaded)
{
    if (!KDMAPI_Setup())
    {
        printf("\nThis program requires OmniMIDI or a KDMAPI capable synth to have functioning audio!\n");
        return 0;
    }
    KDMAPI_InitializeKDMAPIStream();
    printf("singlethread is %s.\n", singlethreaded ? "TRUE" : "FALSE");
    singlethread = singlethreaded;
    if (!singlethreaded && !ring)
    {
        ring = malloc(RINGBUFFER_SIZE);
        packet = malloc(RINGBUFFER_SIZE);
    }
    voicefetching = hasvoice;
    issynthinitiated = 1;
    return 1;
}

static void terminate_stream(void)
{
    if (!issynthinitiated) return;
    KDMAPI_TerminateKDMAPIStream();
    issynthinitiated = 0;
    voicefetching = 0;
}

void Sound_Close(void)
{
    terminate_stream();
    free(ring);
    free(packet);
    ring = packet = NULL;
}

void AllNotesOFF(void)
{
    for (int channel = 0; channel < 16; channel++)
        KDMAPI_SendDirectData((uint32_t)(0xB0 | channel) | (0x7B << 8));
}

// keeps the ring alive. the audio thread may still be reading it while the synth restarts
void reinitsynths(void)
{
    terminate_stream();
    Sound_Init(singlethread);
}
// also thank conmidi for existing
#include "sound.h"
#include "../playback/playback_thread.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

uint24_t* ringbuffer = NULL;
int issynthinitiated = 0, singlethread = 0, voicefetching = 0;
_Atomic uint32_t writeptr = 0;
static pthread_t audio_thread;

static void* audiothread(void* args)
{
    puts("audio thread intialized");
    uint32_t readptr = 0;
    while (!stopping)
    {
        // acquire pairs with the release store in StartPlayback, so the events are visible before the index is
        uint32_t wp = atomic_load_explicit(&writeptr, memory_order_acquire);
        while (readptr != wp && !stopping)
        {
            int32_t val = uint24_get(ringbuffer + readptr);
            if (val)
                KDMAPI_SendDirectData((uint32_t)val);
            readptr = (readptr + 1) & RINGBUFFER_MASK;
        }
    }
    return NULL;
}

void audiothread_entry(void)
{
    atomic_store(&writeptr, 0);
    pthread_create(&audio_thread, NULL, audiothread, NULL);
}

// must run before AllNotesOFF, otherwise the audio thread can still push events after it
void audiothread_join(void)
{
    pthread_join(audio_thread, NULL);
    memset(ringbuffer, 0, RINGBUFFER_SIZE * sizeof(uint24_t));
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
    if (!singlethreaded && !ringbuffer)
        ringbuffer = (uint24_t*)calloc(RINGBUFFER_SIZE, sizeof(uint24_t));
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
    free(ringbuffer);
    ringbuffer = NULL;
}

void AllNotesOFF(void)
{
    for (int channel = 0; channel < 16; channel++)
        KDMAPI_SendDirectData((uint32_t)(0xB0 | channel) | (0x7B << 8));
}

// keeps the ring buffer alive. the audio thread may still be reading it while the synth restarts
void reinitsynths(void)
{
    terminate_stream();
    Sound_Init(singlethread);
}

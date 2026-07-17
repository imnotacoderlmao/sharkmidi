// also thank conmidi for existing
#include "KDMAPI.h"
#include "../misc/uint24.h"
#include "../playback/playback_thread.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
void (*SendDirectData)(uint32_t message);
#if defined(_WIN32) || defined(_WIN64)
    int32_t (*SendDirectLongData)(MIDIHDR* hdr, int32_t hdrsize);    
    int32_t (*PrepareLongData)(MIDIHDR* hdr, int32_t hdrsize);
    int32_t (*UnprepareLongData)(MIDIHDR* hdr, int32_t hdrsize);
#else
    int32_t (*SendDirectLongData)(uint8_t* message, int32_t messagesize);    
#endif
int32_t (*GetVoiceCount)(void);
uint24_t* ringbuffer = NULL;
int32_t voicefetching = 0;
volatile uint32_t writeptr = 0;
volatile uint32_t readptr = 0;
const uint32_t RINGBUFFER_SIZE = 8388608;
const uint32_t RINGBUFFER_MASK = RINGBUFFER_SIZE - 1;
pthread_t audio_thread;

void* audiothread(void* args)
{
    puts("audio thread intialized");
    while(!stopping)
    {
        while (readptr != writeptr)
        {
            int32_t val = uint24_get(ringbuffer + readptr);
            SendDirectData((uint32_t)val);
            readptr = (readptr + 1) & RINGBUFFER_MASK;
        }
    }
    free(ringbuffer);
    pthread_exit(NULL);
    return NULL;
}

int32_t Sound_Init(int32_t singlethread) 
{
    if(!KDMAPI_Setup()){ printf("\nThis program requires OmniMIDI to have functioning audio!\n"); return 0; };

    KDMAPI_InitializeKDMAPIStream();
    SendDirectData = KDMAPI_SendDirectData;
    SendDirectLongData = KDMAPI_SendDirectLongData;
    GetVoiceCount = KDMAPI_GetVoiceCount;
    #if defined(_WIN32) || defined(_WIN64)
        PrepareLongData = KDMAPI_PrepareLongData;
        UnprepareLongData = KDMAPI_UnprepareLongData;
    #endif
    if(hasvoice)
        voicefetching = 1;
    if(!singlethread)
    {
        ringbuffer = (uint24_t*)calloc(RINGBUFFER_SIZE, sizeof(uint24_t));
        pthread_create(&audio_thread, NULL, audiothread, NULL);
        //pthread_join(audio_thread, NULL);
    }
    return 1;
}

void AllNotesOFF(void)
{
    for (int channel = 0; channel < 16; channel++)
        SendDirectData((uint32_t)(0xB0 | channel) | (0x7B << 8));
}
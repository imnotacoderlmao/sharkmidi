#include <stdint.h>
#ifndef KDMAPI_H
#define KDMAPI_H
#if defined(_WIN32) || defined(_WIN64)
    typedef struct {
        uint8_t* lpData;
        uint32_t dwBufferLength;
        uint32_t dwBytesRecorded;
        void* dwUser;
        uint32_t dwFlags;
    } MIDIHDR;
    typedef int32_t (*KDM_LSEND)(MIDIHDR* message, int32_t headersize);
    extern KDM_LSEND KDMAPI_PrepareLongData;
    extern KDM_LSEND KDMAPI_UnprepareLongData;
#else
    typedef int32_t (*KDM_LSEND)(uint8_t* message, int32_t messagesize);
#endif

typedef uint32_t(*KDM_INIT)(void);
typedef int32_t(*KDM_DEBUG)(void);
typedef void (*KDM_SEND)(uint32_t message);

extern KDM_INIT KDMAPI_InitializeKDMAPIStream;
extern KDM_INIT KDMAPI_TerminateKDMAPIStream;
extern KDM_INIT KDMAPI_ResetKDMAPIStream;
extern KDM_DEBUG KDMAPI_GetVoiceCount;
extern KDM_SEND KDMAPI_SendDirectData;
extern KDM_LSEND KDMAPI_SendDirectLongData;
extern int32_t hasvoice;

int32_t KDMAPI_Setup();
#endif
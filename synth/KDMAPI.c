#include <stdio.h>
#include <stdint.h>
#include "KDMAPI.h"

LIB_HANDLE KDMAPI_libHandle;
KDM_INIT KDMAPI_InitializeKDMAPIStream;
KDM_INIT KDMAPI_TerminateKDMAPIStream;
KDM_DEBUG KDMAPI_GetVoiceCount;
KDM_SEND KDMAPI_SendDirectData;
KDM_LSEND KDMAPI_SendDirectLongData;
#if defined(_WIN32) || defined(_WIN64)
KDM_LSEND KDMAPI_PrepareLongData;
KDM_LSEND KDMAPI_UnprepareLongData;
#endif
int32_t hasvoice = 0;

int32_t KDMAPI_Setup()
{
#if defined(_WIN32) || defined(_WIN64)
    KDMAPI_libHandle = LOAD_LIB("OmniMIDI");    
    if ((KDMAPI_PrepareLongData = (KDM_LSEND)GET_FUNC(KDMAPI_libHandle, "PrepareLongData")) == NULL)
        {puts("[KDMAPI] GetProcAddress failed for PrepareLongData."); return 0;}
    if ((KDMAPI_UnprepareLongData = (KDM_LSEND)GET_FUNC(KDMAPI_libHandle, "UnprepareLongData")) == NULL)
        {puts("[KDMAPI] GetProcAddress failed for UnprepareLongData."); return 0;}
#else
    KDMAPI_libHandle = LOAD_LIB("libOmniMIDI.so");        
#endif
    if (!KDMAPI_libHandle)
        {puts("KDMAPI Load failed!"); return 0;}
    if ((KDMAPI_InitializeKDMAPIStream = (KDM_INIT)GET_FUNC(KDMAPI_libHandle, "InitializeKDMAPIStream")) == NULL) 
        {puts("[KDMAPI] GetProcAddress failed for InitializeKDMAPIStream."); return 0;}
    if ((KDMAPI_SendDirectData = (KDM_SEND)GET_FUNC(KDMAPI_libHandle, "SendDirectData")) == NULL)
        {puts("[KDMAPI] GetProcAddress failed for SendDirectData."); return 0;}
    if ((KDMAPI_SendDirectLongData = (KDM_LSEND)GET_FUNC(KDMAPI_libHandle, "SendDirectLongData")) == NULL)
        {puts("[KDMAPI] GetProcAddress failed for SendDirectLongData."); return 0;}
    if ((KDMAPI_TerminateKDMAPIStream = (KDM_INIT)GET_FUNC(KDMAPI_libHandle, "TerminateKDMAPIStream")) == NULL)
        {puts("[KDMAPI] GetProcAddress failed for TerminateKDMAPIStream."); return 0;}
    if ((KDMAPI_GetVoiceCount = (KDM_DEBUG)GET_FUNC(KDMAPI_libHandle, "GetVoiceCount")) != NULL)
        {puts("this omnimidi has voicecount enabled! outputting in playback stats"); hasvoice = 1; }
    puts("KDMAPI loaded!");
    return 1;
}
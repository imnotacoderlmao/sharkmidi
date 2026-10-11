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
int hasvoice = 0;

#define REQUIRE_SYM(var, type, name)                                      \
    if (((var) = (type)GET_FUNC(KDMAPI_libHandle, name)) == NULL)         \
    {                                                                     \
        puts("[KDMAPI] GetProcAddress failed for " name ".");             \
        goto fail;                                                        \
    }

int KDMAPI_Setup(void)
{
    if (KDMAPI_libHandle)
        return 1; // already loaded (synth reinit), the stream is restarted by the caller

#if defined(_WIN32) || defined(_WIN64)
    KDMAPI_libHandle = LOAD_LIB("OmniMIDI");
#else
    KDMAPI_libHandle = LOAD_LIB("libOmniMIDI.so");
#endif
    if (!KDMAPI_libHandle)
    {
        puts("KDMAPI Load failed!");
        return 0;
    }

    REQUIRE_SYM(KDMAPI_InitializeKDMAPIStream, KDM_INIT, "InitializeKDMAPIStream");
    REQUIRE_SYM(KDMAPI_TerminateKDMAPIStream, KDM_INIT, "TerminateKDMAPIStream");
    REQUIRE_SYM(KDMAPI_SendDirectData, KDM_SEND, "SendDirectData");
    REQUIRE_SYM(KDMAPI_SendDirectLongData, KDM_LSEND, "SendDirectLongData");
#if defined(_WIN32) || defined(_WIN64)
    REQUIRE_SYM(KDMAPI_PrepareLongData, KDM_LSEND, "PrepareLongData");
    REQUIRE_SYM(KDMAPI_UnprepareLongData, KDM_LSEND, "UnprepareLongData");
#endif
    // optional
    if ((KDMAPI_GetVoiceCount = (KDM_DEBUG)GET_FUNC(KDMAPI_libHandle, "GetVoiceCount")) != NULL)
    {
        puts("this omnimidi has voicecount enabled! outputting in playback stats");
        hasvoice = 1;
    }
    puts("KDMAPI loaded!");
    return 1;

fail:
    FREE_LIB(KDMAPI_libHandle);
    KDMAPI_libHandle = NULL;
    return 0;
}
#undef REQUIRE_SYM

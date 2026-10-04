#include <stdint.h>
#ifndef KDMAPI_H
#define KDMAPI_H
#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #if !defined(_MMEAPI_H_) && !defined(MIDIHDR)
        typedef struct {
            uint8_t* lpData;
            uint32_t dwBufferLength;
            uint32_t dwBytesRecorded;
            void* dwUser;
            uint32_t dwFlags;
        } MIDIHDR;
    #endif
    #define LIB_HANDLE HMODULE
    #define LOAD_LIB(path) LoadLibraryA(path)
    #define GET_FUNC(handle, name) GetProcAddress(handle, name)
    #define FREE_LIB(handle) FreeLibrary(handle)
    #define GET_ERROR() "Failed to load library or symbol"
    typedef int32_t (*KDM_LSEND)(MIDIHDR *hdr, int32_t headersize);
    extern KDM_LSEND KDMAPI_PrepareLongData;
    extern KDM_LSEND KDMAPI_UnprepareLongData;
#else
    #include <dlfcn.h>
    #define LIB_HANDLE void*
    #define LOAD_LIB(path) dlopen(path, RTLD_LAZY)
    #define GET_FUNC(handle, name) dlsym(handle, name)
    #define FREE_LIB(handle) dlclose(handle)
    #define GET_ERROR() dlerror()
    typedef int32_t(*KDM_LSEND)(uint8_t* message, int32_t messagesize);    
#endif


typedef int32_t(*KDM_INIT)(void);
typedef void(*KDM_NORET)(void);
typedef int32_t(*KDM_DEBUG)(void);
typedef void(*KDM_SEND)(uint32_t message);

extern KDM_INIT KDMAPI_InitializeKDMAPIStream;
extern KDM_INIT KDMAPI_TerminateKDMAPIStream;
extern KDM_DEBUG KDMAPI_GetVoiceCount;
extern KDM_SEND KDMAPI_SendDirectData;
extern KDM_LSEND KDMAPI_SendDirectLongData;
extern int32_t hasvoice;

int32_t KDMAPI_Setup();
#endif
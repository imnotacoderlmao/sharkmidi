#ifndef renderer_hdr
#define renderer_hdr
#include <stdint.h>
#include "../third_party/glad/include/glad/glad.h"

GLuint build_shader(const char* vert, const char* frag);
// the patch scatter needs a few GL functions the glad in the project may not have, they are fetched through this
// (glfwGetProcAddress). call it before Renderer_Init. without it, or when the GPU can't do it, the ends of the notes are
// written one at a time, which is slow
void Renderer_SetProcLoader(void* (*loader)(const char*));
void Renderer_Init(void);
void Renderer_InitForMIDI(void);
void Renderer_ResetForUnload(void);
void Renderer_Dispose(void);
void Renderer_Render(int32_t tick, int pad);

extern int WindowTicks;
extern int NotesDrawnLastFrame;
extern int RingCap;
extern int EnableGlow;
extern int EnableTransparency;
extern int UseForceCull;
extern int stationarynotes;
extern int UseDepthOrder; // 1: the higher track in front whatever the start tick, 0: later start in front
extern int StationaryBars; // measures per page in stationary mode
extern int fbWidth, fbHeight;
#endif
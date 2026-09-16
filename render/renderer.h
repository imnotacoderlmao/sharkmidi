#ifndef renderer_hdr
#define renderer_hdr
#include <stdint.h>

void Renderer_Init(void);
void Renderer_InitForMIDI(void);
void Renderer_ResetForUnload(void);
void Renderer_Dispose(void);
void Renderer_Render(int screenWidth, int screenHeight, int32_t tick, int pad);

extern int WindowTicks;
extern int NotesDrawnLastFrame;
extern int RingCap;
extern int EnableGlow;
extern int EnableTransparency;
extern int UseForceCull;

#endif
#ifndef renderer_hdr
#define renderer_hdr
#include <stdint.h>
#include "../third_party/glad/include/glad/glad.h"

void Renderer_Init(void);
void Renderer_InitForMIDI(void);
void Renderer_ResetForUnload(void);
void Renderer_Dispose(void);
void Renderer_Render(int32_t tick, int pad);

extern int WindowTicks;
extern int NotesDrawnLastFrame;
extern int EnableGlow;
extern int EnableTransparency;
extern int stationarynotes;
extern int StationaryBars; // measures per page in stationary mode
extern int fbWidth, fbHeight;
#endif
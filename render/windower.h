#ifndef window_hdr
#define window_hdr
int Window_Init(int width, int height, const char* title);
void Window_Run(char* filepath); // blocks until the window closes
void Window_Shutdown(void);
#endif
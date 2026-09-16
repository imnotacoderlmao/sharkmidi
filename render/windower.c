#include "windower.h"
#include "renderer.h"
#include "../playback/playback_thread.h"
#include "../parse/parser.h"
#include "../third_party/glad/include/glad/glad.h"
#include "../synth/sound.h"
#include <GLFW/glfw3.h>
#include <stdio.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static HANDLE playbackThread;
static DWORD WINAPI playback_thread_entry(LPVOID arg)
{
    StartPlayback((int)(intptr_t)arg);
    return 0;
}
static void spawn_playback_thread(int singlethread)
{
    playbackThread = CreateThread(NULL, 0, playback_thread_entry, (LPVOID)(intptr_t)singlethread, 0, NULL);
}
#else
#include <pthread.h>
static pthread_t playbackThread;
static void* playback_thread_entry(void* arg)
{
    StartPlayback((int)(intptr_t)arg);
    return NULL;
}
static void spawn_playback_thread(int singlethread)
{
    pthread_create(&playbackThread, NULL, playback_thread_entry, (void*)(intptr_t)singlethread);
}
#endif

static GLFWwindow* win;

static void glfw_error_callback(int code, const char* desc)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, desc);
}

static void key_callback(GLFWwindow* w, int key, int scancode, int action, int mods)
{
    if (action != GLFW_PRESS) return;
    switch (key)
    {
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(w, GLFW_TRUE);
            break;
        case GLFW_KEY_SPACE:
            if (paused) clock_resume(); else clock_pause();
            break;
        case GLFW_KEY_RIGHT:
            clock_skip(tickscale, 0);
            break;
        case GLFW_KEY_LEFT:
            clock_skip(tickscale * -1, 0);
            break;
        case GLFW_KEY_UP:
            WindowTicks *= 1.1;
            break;
        case GLFW_KEY_DOWN:
            WindowTicks /= 1.1;
            break;
    }
}

int Window_Init(int width, int height, const char* title)
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
    {
        fprintf(stderr, "glfwInit failed\n");
        return 0;
    }

    // requesting exactly the 4.2 core floor the renderer targets
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // note: macOS caps at 4.1 regardless, see caveat below
#endif

    win = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!win)
    {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 0;
    }

    glfwMakeContextCurrent(win);
    glfwSwapInterval(1); // vsync; set 0 if you want to test uncapped throughput

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        fprintf(stderr, "gladLoadGLLoader failed\n");
        glfwTerminate();
        return 0;
    }

    glfwSetKeyCallback(win, key_callback);

    printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    return 1;
}

void Window_Run(char* filepath)
{
    Renderer_Init();

    // TODO: replace with real file selection once a GUI exists
    if (LoadMIDI(filepath))
    {
        Renderer_InitForMIDI();
        Sound_Init(1);
        spawn_playback_thread(1); // 0 = multithreaded audio path (ring buffer), 1 = singlethread direct-send
    }

    const int PAD = 20; // placeholder until a real layout/GUI exists

    while (!glfwWindowShouldClose(win))
    {
        int fbWidth, fbHeight;
        glfwGetFramebufferSize(win, &fbWidth, &fbHeight);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (midiloaded)
            Renderer_Render(fbWidth, fbHeight, current_clock, PAD);

        glfwSwapBuffers(win);
        glfwPollEvents();
    }

    stopping = 1; // signal playback_thread's loop to exit
#if defined(_WIN32) || defined(_WIN64)
    WaitForSingleObject(playbackThread, 1000);
#else
    pthread_join(playbackThread, NULL);
#endif
}

void Window_Shutdown(void)
{
    Renderer_Dispose();
    UnloadMIDI();
    glfwDestroyWindow(win);
    glfwTerminate();
}
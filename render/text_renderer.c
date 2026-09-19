#include "text_renderer.h"
#include "renderer.h"
#include "../third_party/glad/include/glad/glad.h"
#define STB_EASY_FONT_IMPLEMENTATION
#include "../third_party/stb_easy_font.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_QUADS 4096

static const char* TextVertSrc =
"#version 420 core\n"
"layout(location=0) in vec3 aPos;\n"
"layout(location=1) in vec4 aColor;\n"
"uniform vec2 uScreenSize;\n"
"uniform vec2 uPos;\n"
"uniform float uScale;\n"
"out vec4 vColor;\n"
"void main() {\n"
"    vec2 pixelPos = uPos + aPos.xy * uScale;\n"
"    float x = (pixelPos.x / uScreenSize.x) * 2.0 - 1.0;\n"
"    float y = 1.0 - (pixelPos.y / uScreenSize.y) * 2.0;\n"
"    gl_Position = vec4(x, y, 0.0, 1.0);\n"
"    vColor = aColor;\n"
"}\n";

static const char* TextFragSrc =
"#version 420 core\n"
"in vec4 vColor;\n"
"out vec4 fragColor;\n"
"void main() { fragColor = vColor; }\n";

typedef struct { float x, y, z; unsigned char color[4]; } TextVertex;

static GLuint textShader;
static GLint u_screenSize, u_pos, u_scale;
static GLuint textVAO, textVBO, textEBO;
static TextVertex vertexBuf[MAX_QUADS * 4];

void Text_Init(void)
{
    textShader = build_shader(TextVertSrc, TextFragSrc);
    u_screenSize = glGetUniformLocation(textShader, "uScreenSize");
    u_pos = glGetUniformLocation(textShader, "uPos");
    u_scale = glGetUniformLocation(textShader, "uScale");

    glGenVertexArrays(1, &textVAO);
    glGenBuffers(1, &textVBO);
    glGenBuffers(1, &textEBO);

    glBindVertexArray(textVAO);

    glBindBuffer(GL_ARRAY_BUFFER, textVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertexBuf), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TextVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TextVertex), (void*)(3 * sizeof(float)));

    uint32_t* indices = malloc(MAX_QUADS * 6 * sizeof(uint32_t));
    for (int q = 0; q < MAX_QUADS; q++)
    {
        uint32_t base = q * 4;
        indices[q*6+0] = base+0; indices[q*6+1] = base+1; indices[q*6+2] = base+2;
        indices[q*6+3] = base+2; indices[q*6+4] = base+3; indices[q*6+5] = base+0;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, textEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, MAX_QUADS * 6 * sizeof(uint32_t), indices, GL_STATIC_DRAW);
    free(indices);

    glBindVertexArray(0);
}

void Text_Dispose(void)
{
    glDeleteBuffers(1, &textVBO);
    glDeleteBuffers(1, &textEBO);
    glDeleteVertexArrays(1, &textVAO);
    glDeleteProgram(textShader);
}

void Text_Draw(int screenWidth, int screenHeight, float x, float y, float scale, float r, float g, float b, const char* fmt, ...)
{
    char str[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(str, sizeof(str), fmt, args);
    va_end(args);

    unsigned char color[4] = { (unsigned char)(r*255), (unsigned char)(g*255), (unsigned char)(b*255), 255 };
    int numQuads = stb_easy_font_print(0, 0, str, color, vertexBuf, sizeof(vertexBuf));
    if (numQuads <= 0) return;
    if (numQuads > MAX_QUADS) numQuads = MAX_QUADS;

    glBindBuffer(GL_ARRAY_BUFFER, textVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, numQuads * 4 * sizeof(TextVertex), vertexBuf);

    glUseProgram(textShader);
    glUniform2f(u_screenSize, (float)screenWidth, (float)screenHeight);
    glUniform2f(u_pos, x, y);
    glUniform1f(u_scale, scale);

    glBindVertexArray(textVAO);
    glDrawElements(GL_TRIANGLES, numQuads * 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}
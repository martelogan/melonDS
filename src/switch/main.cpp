/*
    Copyright 2018 Hydr8gon

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

// Deal with conflicting typedefs
#define u64 u64_
#define s64 s64_

#include "../Config.h"
#include "../Savestate.h"
#include "../GPU.h"
#include "../NDS.h"
#include "../SPU.h"
#include "../version.h"

using namespace std;

vector<const char*> OptionDisplay =
{
    "Boot game directly",
    "Threaded 3D renderer",
    "Screen rotation",
    "Screen layout",
    "Switch overclock"
};

vector<vector<const char*>> OptionValuesDisplay =
{
    { "Off", "On" },
    { "Off", "On" },
    { "0", "90", "180", "270" },
    { "Natural", "Vertical", "Horizontal" },
    { "1020 MHz", "1224 MHz", "1581 MHz", "1785 MHz" }
};

int *OptionValues[] =
{
    &Config::DirectBoot,
    &Config::Threaded3D,
    &Config::ScreenRotation,
    &Config::ScreenLayout,
    &Config::SwitchOverclock
};

ColorSetId MenuTheme;
unsigned char *Font, *FontColor;

u8 *BufferData;
AudioOutBuffer AudioBuffer, *ReleasedBuffer;

u32 *Framebuffer;
unsigned int TouchBoundLeft, TouchBoundRight, TouchBoundTop, TouchBoundBottom;

Mutex EmuMutex;

EGLDisplay Display;
EGLContext Context;
EGLSurface Surface;
GLuint Program, VertArrayObj, VertBufferObj, Texture;

const int charWidth[] =
{
    11, 10, 11, 20, 19, 28, 25,  7, 12, 12,
    15, 25,  9, 11,  9, 17, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21,  9,  9, 26, 25,
    26, 18, 29, 24, 21, 24, 27, 20, 20, 27,
    24, 10, 17, 21, 16, 31, 27, 29, 20, 29,
    20, 19, 21, 26, 25, 37, 22, 21, 24, 12,
    17, 12, 18, 17, 10, 20, 22, 19, 22, 20,
    10, 22, 20,  9, 12, 19,  9, 30, 20, 22,
    22, 22, 13, 17, 13, 20, 17, 29, 18, 18,
    17, 10,  9, 10, 25, 32, 27, 32,  9, 12
};

typedef struct
{
    float position[2];
    float texcoord[2];
} Vertex;

const char *VertexShader =
    "#version 330 core\n"
    "precision mediump float;"

    "layout (location = 0) in vec2 in_pos;"
    "layout (location = 1) in vec2 in_texcoord;"
    "out vec2 vtx_texcoord;"

    "void main()"
    "{"
        "gl_Position = vec4(in_pos, 0.0, 1.0);"
        "vtx_texcoord = in_texcoord;"
    "}";

const char *FragmentShader =
    "#version 330 core\n"
    "precision mediump float;"

    "in vec2 vtx_texcoord;"
    "out vec4 fragcolor;"
    "uniform sampler2D texdiffuse;"

    "void main()"
    "{"
        "fragcolor = texture(texdiffuse, vtx_texcoord);"
    "}";

void InitRenderer()
{
    EGLConfig config;
    EGLint numconfigs;
    EGLint attributelist[] = { EGL_NONE };

    Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(Display, NULL, NULL);
    eglBindAPI(EGL_OPENGL_API);
    eglChooseConfig(Display, attributelist, &config, 1, &numconfigs);
    Surface = eglCreateWindowSurface(Display, config, (char*)"", NULL);
    Context = eglCreateContext(Display, config, EGL_NO_CONTEXT, attributelist);
    eglMakeCurrent(Display, Surface, Surface, Context);

    gladLoadGL();

    GLint vertshader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertshader, 1, &VertexShader, NULL);
    glCompileShader(vertshader);

    GLint fragshader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragshader, 1, &FragmentShader, NULL);
    glCompileShader(fragshader);

    Program = glCreateProgram();
    glAttachShader(Program, vertshader);
    glAttachShader(Program, fragshader);
    glLinkProgram(Program);

    glDeleteShader(vertshader);
    glDeleteShader(fragshader);

    glGenVertexArrays(1, &VertArrayObj);
    glBindVertexArray(VertArrayObj);

    glGenBuffers(1, &VertBufferObj);
    glBindBuffer(GL_ARRAY_BUFFER, VertBufferObj);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texcoord));
    glEnableVertexAttribArray(1);

    glGenTextures(1, &Texture);
    glBindTexture(GL_TEXTURE_2D, Texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glUseProgram(Program);
}

void DeInitRenderer()
{
    glDeleteTextures(1, &Texture);
    glDeleteBuffers(1, &VertBufferObj);
    glDeleteVertexArrays(1, &VertArrayObj);
    glDeleteProgram(Program);

    eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(Display, Context);
    Context = NULL;
    eglDestroySurface(Display, Surface);
    Surface = NULL;
    eglTerminate(Display);
    Display = NULL;
}

unsigned char *TexFromBMP(string filename)
{
    FILE *bmp = fopen(filename.c_str(), "rb");

    unsigned char info[54];
    fread(info, sizeof(unsigned char), 54, bmp);

    int width = *(int*)&info[18];
    int height = *(int*)&info[22];
    unsigned char *data = new unsigned char[width * height * 3];
    fread(data, sizeof(unsigned char), width * height * 3, bmp);

    fclose(bmp);
    return data;
}

void DrawChar(char c, int x, int y, int size, bool color)
{
    int col = c - 32;
    int row = 9;
    while (col > 9)
    {
        col -= 10;
        row--;
    }

    unsigned char *tex = new unsigned char[48 * 48 * 3];
    if (color)
    {
        for (int i = 0; i < 48; i++)
            memcpy(&tex[i * 48 * 3], &FontColor[((row * 512 + col) * 48 + (i + 32) * 512) * 3], 48 * 3);
    }
    else
    {
        for (int i = 0; i < 48; i++)
            memcpy(&tex[i * 48 * 3], &Font[((row * 512 + col) * 48 + (i + 32) * 512) * 3], 48 * 3);
    }

    Vertex character[] =
    {
        { { -1.0f + ((float)(x + size) / 640), 1.0f - ((float)y / 360)          }, { 1.0f, 1.0f } },
        { { -1.0f + ((float)x / 640),          1.0f - ((float)y / 360)          }, { 0.0f, 1.0f } },
        { { -1.0f + ((float)x / 640),          1.0f - ((float)(y + size) / 360) }, { 0.0f, 0.0f } },
        { { -1.0f + ((float)(x + size) / 640), 1.0f - ((float)(y + size) / 360) }, { 1.0f, 0.0f } }
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof(character), character, GL_DYNAMIC_DRAW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 48, 48, 0, GL_BGR, GL_UNSIGNED_BYTE, tex);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void DrawString(string str, int x, int y, int size, bool color)
{
    const char *s = str.c_str();
    int currentx = x;
    for (unsigned int i = 0; i < strlen(s); i++)
    {
        DrawChar(s[i], currentx, y, size, color);
        currentx += (float)charWidth[s[i] - 32] * size / 48;
    }
}

void DrawStringFromRight(string str, int x, int y, int size, bool color)
{
    const char *s = str.c_str();
    int length = 0;
    for (unsigned int i = 0; i < strlen(s); i++)
        length += (float)charWidth[s[i] - 32] * size / 48;
    DrawString(str, x - length, y, size, color);
}

void DrawLine(int x1, int y1, int x2, int y2, bool color)
{
    Vertex line[] =
    {
        { { -1.0f + (float)x1 / 640, 1.0f - (float)y1 / 360 }, { 0.0f, 0.0f } },
        { { -1.0f + (float)x2 / 640, 1.0f - (float)y2 / 360 }, { 0.0f, 0.0f } }
    };

    unsigned char tex[3];
    if (MenuTheme == ColorSetId_Light)
    {
        if (color)
            memset(tex, 205, 3);
        else
            memset(tex, 45, 3);
    }
    else
    {
        if (color)
            memset(tex, 77, 3);
        else
            memset(tex, 255, 3);
    }

    glBufferData(GL_ARRAY_BUFFER, sizeof(line), line, GL_DYNAMIC_DRAW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_BGR, GL_UNSIGNED_BYTE, tex);
    glDrawArrays(GL_LINES, 0, 2);
}

string Menu()
{
    romfsInit();
    if (MenuTheme == ColorSetId_Light)
    {
        glClearColor((float)235 / 255, (float)235 / 255, (float)235 / 255, 1.0f);
        Font = TexFromBMP("romfs:/lightfont.bmp");
        FontColor = TexFromBMP("romfs:/lightfont-color.bmp");
    }
    else
    {
        glClearColor((float)45 / 255, (float)45 / 255, (float)45 / 255, 1.0f);
        Font = TexFromBMP("romfs:/darkfont.bmp");
        FontColor = TexFromBMP("romfs:/darkfont-color.bmp");
    }
    romfsExit();

    string rompath = "sdmc:/";
    bool options = false;

    while (rompath.find(".nds", (rompath.length() - 4)) == string::npos)
    {
        unsigned int selection = 0;
        vector<string> files;

        DIR *dir = opendir(rompath.c_str());
        dirent *entry;
        while ((entry = readdir(dir)))
        {
            string name = entry->d_name;
            if (entry->d_type == DT_DIR || name.find(".nds", (name.length() - 4)) != string::npos)
                files.push_back(name);
        }
        closedir(dir);
        sort(files.begin(), files.end());

        while (true)
        {
            glClear(GL_COLOR_BUFFER_BIT);

            DrawString("melonDS " MELONDS_VERSION, 72, 30, 42, false);
            DrawLine(30, 88, 1250, 88, false);
            DrawLine(30, 648, 1250, 648, false);
            DrawLine(90, 130, 1190, 130, true);

            hidScanInput();
            u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);

            if (options)
            {
                if (pressed & KEY_A)
                {
                    (*OptionValues[selection])++;
                    if (*OptionValues[selection] >= (int)OptionValuesDisplay[selection].size())
                        *OptionValues[selection] = 0;
                }
                else if (pressed & KEY_UP && selection > 0)
                {
                    selection--;
                }
                else if (pressed & KEY_DOWN && selection < OptionDisplay.size() - 1)
                {
                    selection++;
                }
                else if (pressed & KEY_X)
                {
                    Config::Save();
                    options = false;
                    break;
                }

                for (unsigned int i = 0; i < OptionDisplay.size(); i++)
                {
                    DrawString(OptionDisplay[i], 105, 146 + i * 70, 38, i == selection);
                    DrawStringFromRight(OptionValuesDisplay[i][*OptionValues[i]], 1175, 146 + i * 70, 38, i == selection);
                    DrawLine(90, 200 + i * 70, 1190, 200 + i * 70, true);
                }

                DrawStringFromRight("(X) Files    (A) OK", 1208, 665, 36, false);
            }
            else
            {
                if (pressed & KEY_A && files.size() > 0)
                {
                    rompath += "/" + files[selection];
                    break;
                }
                else if (pressed & KEY_B && rompath != "sdmc:/")
                {
                    rompath = rompath.substr(0, rompath.rfind("/"));
                    break;
                }
                else if (pressed & KEY_UP && selection > 0)
                {
                    selection--;
                }
                else if (pressed & KEY_DOWN && selection < files.size() - 1)
                {
                    selection++;
                }
                else if (pressed & KEY_X)
                {
                    Config::Load();
                    options = true;
                    break;
                }

                for (unsigned int i = 0; i < files.size(); i++)
                {
                    DrawString(files[i], 105, 146 + i * 70, 38, i == selection);
                    DrawLine(90, 200 + i * 70, 1190, 200 + i * 70, true);
                }

                DrawStringFromRight("(X) Options    (B) Back    (A) OK", 1208, 665, 36, false);
            }

            eglSwapBuffers(Display, Surface);
        }
    }

    return rompath;
}

void SetScreenLayout()
{
    float width, height, offset_topX, offset_botX, offset_topY, offset_botY;

    if (Config::ScreenLayout == 0)
        Config::ScreenLayout = (Config::ScreenRotation % 2 == 0) ? 1 : 2;

    if (Config::ScreenLayout == 1)
    {
        height = 1.0f;
        if (Config::ScreenRotation % 2 == 0)
            width = height * 0.75;
        else
            width = height * 0.421875;

        offset_topX = offset_botX = -width / 2;
        offset_topY = height;
        offset_botY = 0.0f;
    }
    else
    {
        if (Config::ScreenRotation % 2 == 0)
        {
            width = 1.0f;
            height = width / 0.75;
        }
        else
        {
            height = 2.0f;
            width = height * 0.421875;
        }

        offset_topX = -width;
        offset_botX = 0.0f;
        offset_topY = offset_botY = height / 2;
    }

    Vertex screens[] =
    {
        { { offset_topX + width, offset_topY - height }, { 1.0f, 1.0f } },
        { { offset_topX,         offset_topY - height }, { 0.0f, 1.0f } },
        { { offset_topX,         offset_topY,         }, { 0.0f, 0.0f } },
        { { offset_topX + width, offset_topY,         }, { 1.0f, 0.0f } },

        { { offset_botX + width, offset_botY - height }, { 1.0f, 1.0f } },
        { { offset_botX,         offset_botY - height }, { 0.0f, 1.0f } },
        { { offset_botX,         offset_botY,         }, { 0.0f, 0.0f } },
        { { offset_botX + width, offset_botY,         }, { 1.0f, 0.0f } }
    };

    if (Config::ScreenRotation == 1 || Config::ScreenRotation == 2)
    {
        Vertex *copy = new Vertex[sizeof(screens) / sizeof(Vertex)];
        memcpy(copy, screens, sizeof(screens));
        memcpy(screens, &copy[4], sizeof(screens) / 2);
        memcpy(&screens[4], copy, sizeof(screens) / 2);
        delete copy;
    }

    TouchBoundLeft = (screens[6].position[0] + 1) * 640;
    TouchBoundRight = (screens[4].position[0] + 1) * 640;
    TouchBoundTop = (-screens[6].position[1] + 1) * 360;
    TouchBoundBottom = (-screens[4].position[1] + 1) * 360;

    for (int i = 0; i < Config::ScreenRotation; i++)
    {
        int size = sizeof(screens[0].position);
        Vertex *copy = new Vertex[sizeof(screens) / sizeof(Vertex)];
        memcpy(copy, screens, sizeof(screens));
        for (int k = 0; k < 8; k += 4)
        {
            memcpy(screens[k    ].position, copy[k + 1].position, size);
            memcpy(screens[k + 1].position, copy[k + 2].position, size);
            memcpy(screens[k + 2].position, copy[k + 3].position, size);
            memcpy(screens[k + 3].position, copy[k    ].position, size);
        }
        delete(copy);
    }

    glBufferData(GL_ARRAY_BUFFER, sizeof(screens), screens, GL_DYNAMIC_DRAW);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void AdvFrame(void *args)
{
    while (true)
    {
        chrono::steady_clock::time_point start = chrono::steady_clock::now();

        mutexLock(&EmuMutex);
        NDS::RunFrame();
        mutexUnlock(&EmuMutex);
        memcpy(Framebuffer, GPU::Framebuffer, 256 * 384 * 4);

        while (chrono::duration_cast<chrono::duration<double>>(chrono::steady_clock::now() - start).count() < (float)1 / 60);
    }
}

void FillAudioBuffer()
{
    s16 buf_in[984 * 2];
    s16 *buf_out = (s16*)BufferData;

    int num_in = SPU::ReadOutput(buf_in, 984);
    int num_out = 1440;

    int margin = 6;
    if (num_in < 984 - margin)
    {
        int last = num_in - 1;
        if (last < 0)
            last = 0;

        for (int i = num_in; i < 984 - margin; i++)
            ((u32*)buf_in)[i] = ((u32*)buf_in)[last];

        num_in = 984 - margin;
    }

    float res_incr = (float)num_in / num_out;
    float res_timer = 0;
    int res_pos = 0;

    for (int i = 0; i < 1440; i++)
    {
        buf_out[i * 2] = buf_in[res_pos * 2];
        buf_out[i * 2 + 1] = buf_in[res_pos * 2 + 1];

        res_timer += res_incr;
        while (res_timer >= 1)
        {
            res_timer--;
            res_pos++;
        }
    }
}

void PlayAudio(void *args)
{
    while (true)
    {
        FillAudioBuffer();
        audoutPlayBuffer(&AudioBuffer, &ReleasedBuffer);
    }
}

int main(int argc, char **argv)
{
    InitRenderer();

    setsysInitialize();
    setsysGetColorSetId(&MenuTheme);
    setsysExit();

    string rompath = Menu();
    string srampath = rompath.substr(0, rompath.rfind(".")) + ".sav";
    string statepath = rompath.substr(0, rompath.rfind(".")) + ".mln";

    Config::Load();
    if (!Config::HasConfigFile("bios7.bin") || !Config::HasConfigFile("bios9.bin") || !Config::HasConfigFile("firmware.bin"))
    {
        glClear(GL_COLOR_BUFFER_BIT);
        DrawString("One or more of the following required files don't exist or couldn't be accessed:", 0, 0, 38, false);
        DrawString("bios7.bin -- ARM7 BIOS", 0, 38, 38, false);
        DrawString("bios9.bin -- ARM9 BIOS", 0, 38 * 2, 38, false);
        DrawString("firmware.bin -- firmware image", 0, 38 * 3, 38, false);
        DrawString("Dump the files from your DS and place them in sdmc:/switch/melonds", 0, 38 * 4, 38, false);
        eglSwapBuffers(Display, Surface);
        while (true);
    }

    appletLockExit();

    pcvInitialize();
    if (Config::SwitchOverclock == 0)
        pcvSetClockRate(PcvModule_Cpu, 1020000000);
    else if (Config::SwitchOverclock == 1)
        pcvSetClockRate(PcvModule_Cpu, 1224000000);
    else if (Config::SwitchOverclock == 2)
        pcvSetClockRate(PcvModule_Cpu, 1581000000);
    else
        pcvSetClockRate(PcvModule_Cpu, 1785000000);

    NDS::Init();
    NDS::LoadROM(rompath.c_str(), srampath.c_str(), Config::DirectBoot);

    SetScreenLayout();

    Thread main;
    threadCreate(&main, AdvFrame, NULL, 0x80000, 0x30, 1);
    threadStart(&main);

    audoutInitialize();
    audoutStartAudioOut();

    BufferData = new u8[(1440 * 2 * 2 + 0xfff) & ~0xfff];
    AudioBuffer.next = NULL;
    AudioBuffer.buffer = BufferData;
    AudioBuffer.buffer_size = (1440 * 2 * 2 + 0xfff) & ~0xfff;
    AudioBuffer.data_size = 1440 * 2 * 2;
    AudioBuffer.data_offset = 0;

    Thread audio;
    threadCreate(&audio, PlayAudio, NULL, 0x80000, 0x30, 0);
    threadStart(&audio);

    Framebuffer = new u32[256 * 384];

    HidControllerKeys keys[] = { KEY_A, KEY_B, KEY_MINUS, KEY_PLUS, KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_ZR, KEY_ZL, KEY_X, KEY_Y };

    while (appletMainLoop())
    {
        hidScanInput();
        u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
        u32 released = hidKeysUp(CONTROLLER_P1_AUTO);

        if (pressed & KEY_L || pressed & KEY_R)
        {
            Savestate* state = new Savestate(const_cast<char*>(statepath.c_str()), pressed & KEY_L);
            if (!state->Error)
            {
                mutexLock(&EmuMutex);
                NDS::DoSavestate(state);
                mutexUnlock(&EmuMutex);
            }
            delete state;
        }

        for (int i = 0; i < 12; i++)
        {
            if (pressed & keys[i])
                NDS::PressKey(i > 9 ? i + 6 : i);
            else if (released & keys[i])
                NDS::ReleaseKey(i > 9 ? i + 6 : i);
        }

        if (hidTouchCount() > 0)
        {
            touchPosition touch;
            hidTouchRead(&touch, 0);

            if (touch.px > TouchBoundLeft && touch.px < TouchBoundRight && touch.py > TouchBoundTop && touch.py < TouchBoundBottom)
            {
                int x, y;
                if (Config::ScreenRotation == 0)
                {
                    x = (touch.px - TouchBoundLeft) * 256.0f / (TouchBoundRight - TouchBoundLeft);
                    y = (touch.py - TouchBoundTop) * 256.0f / (TouchBoundRight - TouchBoundLeft);
                }
                else if (Config::ScreenRotation == 1)
                {
                    x = (touch.py - TouchBoundTop) * 192.0f / (TouchBoundRight - TouchBoundLeft);
                    y = 192 - (touch.px - TouchBoundLeft) * 192.0f / (TouchBoundRight - TouchBoundLeft);
                }
                else if (Config::ScreenRotation == 2)
                {
                    x = (touch.px - TouchBoundLeft) * -256.0f / (TouchBoundRight - TouchBoundLeft);
                    y = 192 - (touch.py - TouchBoundTop) * 256.0f / (TouchBoundRight - TouchBoundLeft);
                }
                else
                {
                    x = (touch.py - TouchBoundTop) * -192.0f / (TouchBoundRight - TouchBoundLeft);
                    y = (touch.px - TouchBoundLeft) * 192.0f / (TouchBoundRight - TouchBoundLeft);
                }
                NDS::PressKey(16 + 6);
                NDS::TouchScreen(x, y);
            }
        }
        else
        {
            NDS::ReleaseKey(16 + 6);
            NDS::ReleaseScreen();
        }

        glClear(GL_COLOR_BUFFER_BIT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, Framebuffer);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, &Framebuffer[256 * 192]);
        glDrawArrays(GL_TRIANGLE_FAN, 4, 4);
        eglSwapBuffers(Display, Surface);
    }

    audoutExit();
    DeInitRenderer();
    pcvSetClockRate(PcvModule_Cpu, 1020000000);
    pcvExit();
    appletUnlockExit();
    return 0;
}

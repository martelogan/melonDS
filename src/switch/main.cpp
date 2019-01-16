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
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>
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
#include "../melon_fopen.h"
#include "../version.h"

using namespace std;

ColorSetId MenuTheme;
unsigned char *Font, *FontColor;

char *EmuDirectory;
string ROMPath, SRAMPath, StatePath, StateSRAMPath;

s16 *AudOutBufferData, *AudInBufferData;
AudioInBuffer AudInBuffer, *RelInBuffer;

u32 *DisplayBuffer;
unsigned int TouchBoundLeft, TouchBoundRight, TouchBoundTop, TouchBoundBottom;
bool Paused, LidClosed;

EGLDisplay Display;
EGLContext Context;
EGLSurface Surface;
GLuint Program, VertArrayObj, VertBufferObj, Texture;

const int ClockSpeeds[] = { 1020000000, 1224000000, 1581000000, 1785000000 };

const int CharWidth[] =
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
    17, 10,  9, 10, 25, 32, 40, 40, 40, 40
};

typedef struct
{
    const char *name;
    vector<const char*> entries;
    int *value;
} Option;

const vector<Option> Options =
{
    { "Boot game directly",                 { "Off", "On" },                                                               &Config::DirectBoot },
    { "Threaded 3D renderer",               { "Off", "On" },                                                               &Config::Threaded3D },
    { "Audio volume",                       { "0%", "25%", "50%", "75%", "100%" },                                         &Config::AudioVolume },
    { "Microphone input",                   { "None", "Microphone", "White noise" },                                       &Config::MicInputType },
    { "Separate savefiles from savestates", { "Off", "On" },                                                               &Config::SavestateRelocSRAM },
    { "Screen rotation",                    { "0", "90", "180", "270" },                                                   &Config::ScreenRotation },
    { "Mid-screen gap",                     { "0 pixels", "1 pixel", "8 pixels", "64 pixels", "90 pixels", "128 pixels" }, &Config::ScreenGap },
    { "Screen layout",                      { "Natural", "Vertical", "Horizontal" },                                       &Config::ScreenLayout },
    { "Screen sizing",                      { "Even", "Emphasize top", "Emphasize bottom" },                               &Config::ScreenSizing },
    { "Screen filtering",                   { "Off", "On" },                                                               &Config::ScreenFilter },
    { "Limit framerate",                    { "Off", "On" },                                                               &Config::LimitFPS },
    { "Switch overclock",                   { "1020 MHz", "1224 MHz", "1581 MHz", "1785 MHz" },                            &Config::SwitchOverclock }
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
        "gl_Position = vec4(-1.0 + in_pos.x / 640, 1.0 - in_pos.y / 360, 0.0, 1.0);"
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

    Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(Display, NULL, NULL);
    eglBindAPI(EGL_OPENGL_API);
    eglChooseConfig(Display, {}, &config, 1, &numconfigs);
    Surface = eglCreateWindowSurface(Display, config, (char*)"", NULL);
    Context = eglCreateContext(Display, config, EGL_NO_CONTEXT, {});
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
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

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
    FILE *bmp = melon_fopen(filename.c_str(), "rb");

    // Deal with the file header
    unsigned char header[54];
    fread(header, sizeof(unsigned char), 54, bmp);
    int width = *(int*)&header[18];
    int height = *(int*)&header[22];

    unsigned char *data = new unsigned char[width * height * 3];
    fread(data, sizeof(unsigned char), width * height * 3, bmp);

    fclose(bmp);
    return data;
}

void DrawString(string str, float x, float y, int size, bool color, bool fromright)
{
    int width = 0;
    for (unsigned int i = 0; i < str.size(); i++)
        width += CharWidth[str[i] - 32];

    unsigned char *font = color ? FontColor : Font;
    unsigned char *tex = new unsigned char[width * 48 * 3];
    int currentx = 0;

    for (unsigned int i = 0; i < str.size(); i++)
    {
        int col = str[i] - 32;
        int row = 9;
        while (col > 9)
        {
            col -= 10;
            row--;
        }

        for (int j = 0; j < 48; j++)
            memcpy(&tex[(j * width + currentx) * 3], &font[((row * 512 + col) * 48 + (j + 32) * 512) * 3], CharWidth[str[i] - 32] * 3);

        currentx += CharWidth[str[i] - 32];
    }

    if (fromright)
        x -= width * size / 48;

    Vertex string[] =
    {
        { { x + width * size / 48, y        }, { 1.0f, 1.0f } },
        { { x,                     y        }, { 0.0f, 1.0f } },
        { { x,                     y + size }, { 0.0f, 0.0f } },
        { { x + width * size / 48, y + size }, { 1.0f, 0.0f } }
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof(string), string, GL_DYNAMIC_DRAW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, 48, 0, GL_BGR, GL_UNSIGNED_BYTE, tex);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    delete[] tex;
}

void DrawLine(float x1, float y1, float x2, float y2, bool color)
{
    Vertex line[] =
    {
        { { x1, y1 }, { 0.0f, 0.0f } },
        { { x2, y2 }, { 0.0f, 0.0f } }
    };

    unsigned char tex[3];
    if (MenuTheme == ColorSetId_Light)
        memset(tex, color ? 205 : 45, sizeof(tex));
    else
        memset(tex, color ? 77 : 255, sizeof(tex));

    glBufferData(GL_ARRAY_BUFFER, sizeof(line), line, GL_DYNAMIC_DRAW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_BGR, GL_UNSIGNED_BYTE, tex);
    glDrawArrays(GL_LINES, 0, 2);
}

void DrawStaticUI()
{
    DrawString("melonDS " MELONDS_VERSION, 72, 30, 42, false, false);
    DrawLine(30, 88, 1250, 88, false);
    DrawLine(30, 648, 1250, 648, false);
}

void OptionsMenu()
{
    unsigned int selection = 0;

    while (true)
    {
        glClear(GL_COLOR_BUFFER_BIT);

        DrawStaticUI();
        DrawLine(90, 124, 1190, 124, true);
        DrawString("� Back     � OK", 1218, 667, 34, false, true);

        hidScanInput();
        u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
        if (pressed & KEY_A)
        {
            if (selection == 2) // Audio volume
            {
                (*Options[selection].value) += 256 / 4;
                if (*Options[selection].value > 256)
                    *Options[selection].value = 0;
            }
            else
            {
                (*Options[selection].value)++;
                if (*Options[selection].value >= (int)Options[selection].entries.size())
                    *Options[selection].value = 0;
            }
        }
        else if (pressed & KEY_B)
        {
            Config::Save();
            break;
        }
        else if (pressed & KEY_UP && selection > 0)
        {
            selection--;
        }
        else if (pressed & KEY_DOWN && selection < Options.size() - 1)
        {
            selection++;
        }

        for (int i = 0; i < 7; i++)
        {
            unsigned int row;
            if (selection < 4)
                row = i;
            else if (selection > Options.size() - 4)
                row = Options.size() - 7 + i;
            else
                row = i + selection - 3;

            string currentvalue;
            if (row == 2) // Audio volume
                currentvalue = Options[row].entries[*Options[row].value * 4 / 256];
            else
                currentvalue = Options[row].entries[*Options[row].value];

            DrawString(Options[row].name, 105, 140 + i * 70, 38, row == selection, false);
            DrawString(currentvalue, 1175, 143 + i * 70, 32, row == selection, true);
            DrawLine(90, 194 + i * 70, 1190, 194 + i * 70, true);
        }

        eglSwapBuffers(Display, Surface);
    }
}

void FilesMenu()
{
    if (MenuTheme == ColorSetId_Light)
        glClearColor(235.0f / 255, 235.0f / 255, 235.0f / 255, 1.0f);
    else
        glClearColor(45.0f / 255, 45.0f / 255, 45.0f / 255, 1.0f);

    if (strcmp(Config::LastROMFolder, "") == 0)
        ROMPath = "sdmc:/";
    else
        ROMPath = Config::LastROMFolder;

    while (ROMPath.find(".nds", (ROMPath.length() - 4)) == string::npos)
    {
        unsigned int selection = 0;
        vector<string> files;

        DIR *dir = opendir(ROMPath.c_str());
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

            DrawStaticUI();
            DrawLine(90, 124, 1190, 124, true);
            DrawString("� Exit     � Options     � Back     � OK", 1218, 667, 34, false, true);

            hidScanInput();
            u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
            if (pressed & KEY_A && files.size() > 0)
            {
                ROMPath += "/" + files[selection];
                break;
            }
            else if (pressed & KEY_B && ROMPath != "sdmc:/")
            {
                ROMPath = ROMPath.substr(0, ROMPath.rfind("/"));
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
                OptionsMenu();
                selection = 0;
            }
            else if (pressed & KEY_PLUS)
            {
                ROMPath = "";
                return;
            }

            for (unsigned int i = 0; i < 7; i++)
            {
                if (i < files.size())
                {
                    unsigned int row;
                    if (selection < 4 || files.size() <= 7)
                        row = i;
                    else if (selection > files.size() - 4)
                        row = files.size() - 7 + i;
                    else
                       row = i + selection - 3;

                    DrawString(files[row], 105, 140 + i * 70, 38, row == selection, false);
                    DrawLine(90, 194 + i * 70, 1190, 194 + i * 70, true);
                }
            }

            eglSwapBuffers(Display, Surface);
        }
    }

    string folder = ROMPath.substr(0, ROMPath.rfind("/")).c_str();
    folder.append(1, '\0');
    strncpy(Config::LastROMFolder, folder.c_str(), folder.length());
}

bool LocalFileExists(const char *name)
{
    FILE *file = melon_fopen_local(name, "rb");
    if (!file)
        return false;
    fclose(file);
    return true;
}

void SetScreenLayout()
{
    float width_top, height_top, width_bot, height_bot, offsetX_top, offsetX_bot, offsetY_top, offsetY_bot, gap;

    int gapsizes[] = { 0, 1, 8, 64, 90, 128 };
    gap = gapsizes[Config::ScreenGap];

    if (Config::ScreenLayout == 0) // Natural, choose based on rotation
        Config::ScreenLayout = (Config::ScreenRotation % 2 == 0) ? 1 : 2;

    if (Config::ScreenLayout == 1) // Vertical
    {
        if (Config::ScreenSizing == 0) // Even
        {
            height_top = height_bot = 360 - gap / 2;
            if (Config::ScreenRotation % 2 == 0)
                width_top = width_bot = height_top * 4 / 3;
            else
                width_top = width_bot = height_top * 3 / 4;
        }
        else if (Config::ScreenSizing == 1) // Emphasize top
        {
            if (Config::ScreenRotation % 2 == 0)
            {
                width_bot = 256;
                height_bot = 192;
                height_top = 720 - height_bot - gap;
                width_top = height_top * 4 / 3;
            }
            else
            {
                width_bot = 192;
                height_bot = 256;
                height_top = 720 - height_bot - gap;
                width_top = height_top * 3 / 4;
            }
        }
        else // Emphasize bottom
        {
            if (Config::ScreenRotation % 2 == 0)
            {
                width_top = 256;
                height_top = 192;
                height_bot = 720 - height_top - gap;
                width_bot = height_bot * 4 / 3;
            }
            else
            {
                width_bot = 192;
                height_bot = 256;
                height_top = 720 - height_bot - gap;
                width_top = height_top * 3 / 4;
            }
        }

        offsetX_top = 640 - width_top / 2;
        offsetX_bot = 640 - width_bot / 2;
        offsetY_top = 0;
        offsetY_bot = 720 - height_bot;
    }
    else // Horizontal
    {
        if (Config::ScreenRotation % 2 == 0)
        {
            width_top = width_bot = 640 - gap / 2;
            height_top = height_bot = width_top * 3 / 4;
            offsetX_top = 0;
            offsetX_bot = 1280 - width_top;
        }
        else
        {
            height_top = height_bot = 720;
            width_top = width_bot = height_top * 3 / 4;
            offsetX_top = 640 - width_top - gap / 2;
            offsetX_bot = 640 + gap / 2;
        }

        offsetY_top = offsetY_bot = 360 - height_top / 2;

        if (Config::ScreenSizing == 1) // Emphasize top
        {
            if (Config::ScreenRotation % 2 == 0)
            {
                width_bot = 256;
                height_bot = 192;
                width_top = 1280 - width_bot - gap;
                if (width_top > 960)
                    width_top = 960;
                height_top = width_top * 3 / 4;
                offsetX_top = 640 - (width_bot + width_top + gap) / 2;
                offsetX_bot = offsetX_top + width_top + gap;
                offsetY_top = 360 - height_top / 2;
                offsetY_bot = offsetY_top + height_top - height_bot;
            }
            else
            {
                width_bot = 192;
                height_bot = 256;
                offsetX_top += (width_top - width_bot) / 2;
                offsetX_bot += (width_top - width_bot) / 2;
                offsetY_bot = 720 - height_bot;
            }
        }
        else if (Config::ScreenSizing == 2) // Emphasize bottom
        {
            if (Config::ScreenRotation % 2 == 0)
            {
                width_top = 256;
                height_top = 192;
                width_bot = 1280 - width_top - gap;
                if (width_bot > 960)
                    width_bot = 960;
                height_bot = width_bot * 3 / 4;
                offsetX_top = 640 - (width_bot + width_top + gap) / 2;
                offsetX_bot = offsetX_top + width_top + gap;
                offsetY_bot = 360 - height_bot / 2;
                offsetY_top = offsetY_bot + height_bot - height_top;
            }
            else
            {
                width_top = 192;
                height_top = 256;
                offsetX_top += (width_bot - width_top) / 2;
                offsetX_bot -= (width_bot - width_top) / 2;
                offsetY_top = 720 - height_top;
            }
        }
    }

    Vertex screens[] =
    {
        { { offsetX_top + width_top, offsetY_top + height_top }, { 1.0f, 1.0f } },
        { { offsetX_top,             offsetY_top + height_top }, { 0.0f, 1.0f } },
        { { offsetX_top,             offsetY_top              }, { 0.0f, 0.0f } },
        { { offsetX_top + width_top, offsetY_top              }, { 1.0f, 0.0f } },

        { { offsetX_bot + width_bot, offsetY_bot + height_bot }, { 1.0f, 1.0f } },
        { { offsetX_bot,             offsetY_bot + height_bot }, { 0.0f, 1.0f } },
        { { offsetX_bot,             offsetY_bot              }, { 0.0f, 0.0f } },
        { { offsetX_bot + width_bot, offsetY_bot              }, { 1.0f, 0.0f } }
    };

    // Swap top and bottom screens for 90 and 180 degrees
    if (Config::ScreenRotation == 1 || Config::ScreenRotation == 2)
    {
        Vertex *copy = new Vertex[sizeof(screens) / sizeof(Vertex)];
        memcpy(copy, screens, sizeof(screens));
        memcpy(screens, &copy[4], sizeof(screens) / 2);
        memcpy(&screens[4], copy, sizeof(screens) / 2);
        delete[] copy;
    }

    TouchBoundLeft = screens[6].position[0];
    TouchBoundRight = screens[4].position[0];
    TouchBoundTop = screens[6].position[1];
    TouchBoundBottom = screens[4].position[1];

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
        delete[] copy;
    }

    if (!Config::ScreenFilter)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    glBufferData(GL_ARRAY_BUFFER, sizeof(screens), screens, GL_DYNAMIC_DRAW);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void RunCore(void *args)
{
    while (!Paused)
    {
        chrono::steady_clock::time_point start = chrono::steady_clock::now();

        NDS::RunFrame();
        memcpy(DisplayBuffer, GPU::Framebuffer, sizeof(GPU::Framebuffer));

        chrono::duration<double> elapsed = chrono::steady_clock::now() - start;
        if (Config::LimitFPS && elapsed.count() < 1.0f / 60)
            usleep((1.0f / 60 - elapsed.count()) * 1000000);
    }
}

void FillAudioBuffer()
{
    // 1024 samples is about 1400 at the original sample rate

    s16 buf_in[1400 * 2];
    s16 *buf_out = AudOutBufferData;

    int num_in = SPU::ReadOutput(buf_in, 1400);
    int num_out = 1024;

    int margin = 6;
    if (num_in < 1400 - margin)
    {
        int last = num_in - 1;
        if (last < 0)
            last = 0;

        for (int i = num_in; i < 1400 - margin; i++)
            ((u32*)buf_in)[i] = ((u32*)buf_in)[last];

        num_in = 1400 - margin;
    }

    float res_incr = (float)num_in / num_out;
    float res_timer = 0;
    int res_pos = 0;

    for (int i = 0; i < 1024; i++)
    {
        buf_out[i * 2] = (buf_in[res_pos * 2] * Config::AudioVolume) >> 8;
        buf_out[i * 2 + 1] = (buf_in[res_pos * 2 + 1] * Config::AudioVolume) >> 8;

        res_timer += res_incr;
        while (res_timer >= 1)
        {
            res_timer--;
            res_pos++;
        }
    }
}

void MicInput(void *args)
{
    while (!Paused)
    {
        if (Config::MicInputType == 0)
        {
            NDS::MicInputFrame(NULL, 0);
        }
        else if (Config::MicInputType == 1)
        {
            audinCaptureBuffer(&AudInBuffer, &RelInBuffer);
            NDS::MicInputFrame(AudInBufferData, 1440);
        }
        else
        {
            s16 input[1440];
            for (int i = 0; i < 1440; i++)
                input[i] = rand() & 0xFFFF;
            NDS::MicInputFrame(input, 1440);
        }
    }
}

void StartCore(bool resume)
{
    SRAMPath = ROMPath.substr(0, ROMPath.rfind(".")) + ".sav";
    StatePath = ROMPath.substr(0, ROMPath.rfind(".")) + ".mln";
    StateSRAMPath = StatePath + ".sav";

    appletLockExit();
    if (Config::MicInputType == 1)
    {
        audinInitialize();
        audinStartAudioIn();
    }
    if (Config::SwitchOverclock > 0)
    {
        pcvInitialize();
        pcvSetClockRate(PcvModule_Cpu, ClockSpeeds[Config::SwitchOverclock]);
    }
    Paused = false;

    if (!resume)
    {
        NDS::Init();
        NDS::LoadROM(ROMPath.c_str(), SRAMPath.c_str(), Config::DirectBoot);
    }

    SetScreenLayout();

    Thread core, mic;
    threadCreate(&core, RunCore, NULL, 0x80000, 0x30, 1);
    threadStart(&core);
    threadCreate(&mic, MicInput, NULL, 0x80000, 0x30, 0);
    threadStart(&mic);
}

void Pause()
{
    Paused = true;
    pcvSetClockRate(PcvModule_Cpu, ClockSpeeds[0]);
    pcvExit();
    audinStopAudioIn();
    audinExit();
    appletUnlockExit();
}

void PauseMenu()
{
    Pause();

    if (MenuTheme == ColorSetId_Light)
        glClearColor(235.0f / 255, 235.0f / 255, 235.0f / 255, 1.0f);
    else
        glClearColor(45.0f / 255, 45.0f / 255, 45.0f / 255, 1.0f);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    vector<const char*> items = 
    {
        "Resume",
        LidClosed ? "Open lid" : "Close lid",
        "Save state",
        "Load state",
        "Options",
        "File browser"
    };

    while (Paused)
    {
        unsigned int selection = 0;

        while (true)
        {
            glClear(GL_COLOR_BUFFER_BIT);
            DrawStaticUI();
            DrawLine(90, 124, 1190, 124, true);
            DrawString("� OK", 1218, 667, 34, false, true);
            for (unsigned int i = 0; i < items.size(); i++)
            {
                DrawString(items[i], 105, 140 + i * 70, 38, i == selection, false);
                DrawLine(90, 194 + i * 70, 1190, 194 + i * 70, true);
            }
            eglSwapBuffers(Display, Surface);

            hidScanInput();
            u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
            if (pressed & KEY_A)
                break;
            else if (pressed & KEY_UP && selection > 0)
                selection--;
            else if (pressed & KEY_DOWN && selection < items.size() - 1)
                selection++;
        }

        if (selection == 0) // Resume
        {
            StartCore(true);
        }
        else if (selection == 1) // Open/close lid
        {
            LidClosed = !LidClosed;
            NDS::SetLidClosed(LidClosed);
            StartCore(true);
        }
        else if (selection == 2 || selection == 3) // Save/load state
        {
            Savestate* state = new Savestate(const_cast<char*>(StatePath.c_str()), selection == 2);
            if (!state->Error)
            {
                NDS::DoSavestate(state);
                if (Config::SavestateRelocSRAM)
                    NDS::RelocateSave(const_cast<char*>(StateSRAMPath.c_str()), selection == 2);
            }
            delete state;

            StartCore(true);
        }
        else if (selection == 4) // Options
        {
            OptionsMenu();
        }
        else // File browser
        {
            NDS::DeInit();

            FilesMenu();
            if (ROMPath == "")
                break;

            StartCore(false);
        }
    }
}

int main(int argc, char **argv)
{
    InitRenderer();

    setsysInitialize();
    setsysGetColorSetId(&MenuTheme);
    setsysExit();

    romfsInit();
    if (MenuTheme == ColorSetId_Light)
    {
        Font = TexFromBMP("romfs:/lightfont.bmp");
        FontColor = TexFromBMP("romfs:/lightfont-color.bmp");
    }
    else
    {
        Font = TexFromBMP("romfs:/darkfont.bmp");
        FontColor = TexFromBMP("romfs:/darkfont-color.bmp");
    }
    romfsExit();

    AudioRendererConfig arconfig =
    {
        .output_rate     = AudioRendererOutputRate_48kHz,
        .num_voices      = 1,
        .num_effects     = 0,
        .num_sinks       = 1,
        .num_mix_objs    = 1,
        .num_mix_buffers = 2,
    };

    audrenInitialize(&arconfig);
    audrenStartAudioRenderer();

    AudOutBufferData = (s16*)memalign(0x1000, (1024 * sizeof(s16) + 0xfff) & ~0xfff);
    AudioDriverWaveBuf wavebuf = {0};
    wavebuf.data_pcm16 = AudOutBufferData;
    wavebuf.size = 1024 * sizeof(s16);
    wavebuf.start_sample_offset = 0;
    wavebuf.end_sample_offset = 1024;

    AudioDriver drv;
    audrvCreate(&drv, &arconfig, 2);
    audrvMemPoolAttach(&drv, audrvMemPoolAdd(&drv, AudOutBufferData, (1024 * sizeof(s16) + 0xfff) & ~0xfff));
    u8 channels[] = { 0, 1 };
    audrvDeviceSinkAdd(&drv, AUDREN_DEFAULT_DEVICE_NAME, 2, channels);
    audrvUpdate(&drv);

    audrvVoiceInit(&drv, 0, 1, PcmFormat_Int16, 48000);
    audrvVoiceSetDestinationMix(&drv, 0, AUDREN_FINAL_MIX_ID);
    audrvVoiceSetMixFactor(&drv, 0, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&drv, 0, 1.0f, 0, 1);
    audrvVoiceStart(&drv, 0);

    EmuDirectory = (char*)"sdmc:/switch/melonds";
    Config::Load();

    FilesMenu();
    if (ROMPath == "")
    {
        DeInitRenderer();
        return 0;
    }

    if (!LocalFileExists("bios7.bin") || !LocalFileExists("bios9.bin") || !LocalFileExists("firmware.bin"))
    {
        glClear(GL_COLOR_BUFFER_BIT);
        DrawStaticUI();
        DrawString("� Exit", 1218, 667, 34, false, true);
        DrawString("One or more of the following required files don't exist or couldn't be accessed:", 90, 124, 38, false, false);
        DrawString("bios7.bin -- ARM7 BIOS", 90, 124 + 38, 38, false, false);
        DrawString("bios9.bin -- ARM9 BIOS", 90, 124 + 38 * 2, 38, false, false);
        DrawString("firmware.bin -- firmware image", 90, 124 + 38 * 3, 38, false, false);
        DrawString("Dump the files from your DS and place them in sdmc:/switch/melonds", 90, 124 + 38 * 4, 38, false, false);
        eglSwapBuffers(Display, Surface);

        while (true)
        {
            hidScanInput();
            u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
            if (pressed & KEY_PLUS)
            {
                DeInitRenderer();
                return 0;
            }
        }
    }

    AudInBufferData = new s16[(1440 * 2 + 0xfff) & ~0xfff];
    AudInBuffer.next = NULL;
    AudInBuffer.buffer = AudInBufferData;
    AudInBuffer.buffer_size = (1440 * 2 * sizeof(s16) + 0xfff) & ~0xfff;
    AudInBuffer.data_size = 1440 * 2 * sizeof(s16);
    AudInBuffer.data_offset = 0;

    StartCore(false);

    DisplayBuffer = new u32[256 * 384];

    HidControllerKeys keys[] = { KEY_A, KEY_B, KEY_MINUS, KEY_PLUS, KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_ZR, KEY_ZL, KEY_X, KEY_Y };

    while (appletMainLoop())
    {
        // This should be on another thread, but doing so locks up the system??
        if (wavebuf.state != AudioDriverWaveBufState_Playing)
        {
            FillAudioBuffer();
            audrvVoiceAddWaveBuf(&drv, 0, &wavebuf);
            audrvVoiceStart(&drv, 0);
        }
        audrvUpdate(&drv);

        hidScanInput();
        u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
        u32 released = hidKeysUp(CONTROLLER_P1_AUTO);

        if (pressed & KEY_L || pressed & KEY_R)
        {
            PauseMenu();
            if (ROMPath == "")
                break;
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, DisplayBuffer);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, &DisplayBuffer[256 * 192]);
        glDrawArrays(GL_TRIANGLE_FAN, 4, 4);
        eglSwapBuffers(Display, Surface);
    }

    DeInitRenderer();
    Pause();
    audrvClose(&drv);
    audrenExit();
    return 0;
}

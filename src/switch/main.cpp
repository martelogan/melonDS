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

u8 *OutBufferData;
AudioOutBuffer AudOutBuffer, *ReleasedOutBuffer;

u8 *InBufferData;
AudioInBuffer AudInBuffer, *ReleasedInBuffer;

u32 *Framebuffer;
unsigned int TouchBoundLeft, TouchBoundRight, TouchBoundTop, TouchBoundBottom;

Thread Core, Audio, Mic;
bool Paused, LidClosed;

EGLDisplay Display;
EGLContext Context;
EGLSurface Surface;
GLuint Program, VertArrayObj, VertBufferObj, Texture;

int *OptionValues[] =
{
    &Config::DirectBoot,
    &Config::Threaded3D,
    &Config::AudioVolume,
    &Config::MicInputType,
    &Config::SavestateRelocSRAM,
    &Config::ScreenRotation,
    &Config::ScreenGap,
    &Config::ScreenLayout,
    &Config::ScreenSizing,
    &Config::ScreenFilter,
    &Config::LimitFPS,
    &Config::SwitchOverclock
};

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
    17, 10,  9, 10, 25, 32, 40, 40, 40, 40
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
    FILE *bmp = melon_fopen(filename.c_str(), "rb");

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
    const char *s = str.c_str();

    int width = 0;
    for (unsigned int i = 0; i < strlen(s); i++)
        width += charWidth[s[i] - 32];

    // Texture dimensions must be divisible by 4
    int extra = 0;
    while (width % 4 != 0)
    {
        width++;
        extra++;
    }

    unsigned char *tex = new unsigned char[width * 48 * 3];
    int currentx = 0;

    for (unsigned int i = 0; i < strlen(s); i++)
    {
        int col = s[i] - 32;
        int row = 9;
        while (col > 9)
        {
            col -= 10;
            row--;
        }

        int cwidth = charWidth[s[i] - 32];
        if (i == strlen(s) - 1)
            cwidth += extra;

        if (color)
        {
            for (int j = 0; j < 48; j++)
                memcpy(&tex[(j * width + currentx) * 3], &FontColor[((row * 512 + col) * 48 + (j + 32) * 512) * 3], cwidth * 3);
        }
        else
        {
            for (int j = 0; j < 48; j++)
                memcpy(&tex[(j * width + currentx) * 3], &Font[((row * 512 + col) * 48 + (j + 32) * 512) * 3], cwidth * 3);
        }

        currentx += cwidth;
    }

    if (fromright)
        x -= (float)width * size / 48;

    Vertex string[] =
    {
        { { x + (float)width * size / 48, y        }, { 1.0f, 1.0f } },
        { { x,                            y        }, { 0.0f, 1.0f } },
        { { x,                            y + size }, { 0.0f, 0.0f } },
        { { x + (float)width * size / 48, y + size }, { 1.0f, 0.0f } }
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

void OptionsMenu()
{
    vector<const char*> options =
    {
        "Boot game directly",
        "Threaded 3D renderer",
        "Audio volume",
        "Microphone input",
        "Separate savefiles from savestates",
        "Screen rotation",
        "Mid-screen gap",
        "Screen layout",
        "Screen sizing",
        "Screen filtering",
        "Limit framerate",
        "Switch overclock"
    };

    vector<vector<const char*>> possiblevalues =
    {
        { "Off", "On" },
        { "Off", "On" },
        { "0%", "25%", "50%", "75%", "100%" },
        { "None", "Microphone", "White noise" },
        { "Off", "On" },
        { "0", "90", "180", "270" },
        { "0 pixels", "1 pixel", "8 pixels", "64 pixels", "90 pixels", "128 pixels" },
        { "Natural", "Vertical", "Horizontal" },
        { "Even", "Emphasize top", "Emphasize bottom" },
        { "Off", "On" },
        { "Off", "On" },
        { "1020 MHz", "1224 MHz", "1581 MHz", "1785 MHz" }
    };

    unsigned int selection = 0;

    while (true)
    {
        glClear(GL_COLOR_BUFFER_BIT);

        DrawString("melonDS " MELONDS_VERSION, 72, 30, 42, false, false);
        DrawLine(30, 88, 1250, 88, false);
        DrawLine(30, 648, 1250, 648, false);
        DrawLine(90, 124, 1190, 124, true);
        DrawString(" Back     € OK", 1218, 667, 34, false, true);

        hidScanInput();
        u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
        if (pressed & KEY_A)
        {
            if (selection == 2)
            {
                (*OptionValues[selection]) += 256 / 4;
                if (*OptionValues[selection] > 256)
                    *OptionValues[selection] = 0;
            }
            else
            {
                (*OptionValues[selection])++;
                if (*OptionValues[selection] >= (int)possiblevalues[selection].size())
                    *OptionValues[selection] = 0;
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
        else if (pressed & KEY_DOWN && selection < options.size() - 1)
        {
            selection++;
        }

        for (int i = 0; i < 7; i++)
        {
            unsigned int row;
            if (selection < 4)
                row = i;
            else if (selection > options.size() - 4)
                row = options.size() - 7 + i;
            else
                row = i + selection - 3;

            string currentvalue;
            if (row == 2)
                currentvalue = possiblevalues[row][*OptionValues[row] * 4 / 256];
            else
                currentvalue = possiblevalues[row][*OptionValues[row]];

            DrawString(options[row], 105, 140 + i * 70, 38, row == selection, false);
            DrawString(currentvalue, 1175, 143 + i * 70, 32, row == selection, true);
            DrawLine(90, 194 + i * 70, 1190, 194 + i * 70, true);
        }

        eglSwapBuffers(Display, Surface);
    }
}

void FilesMenu()
{
    if (MenuTheme == ColorSetId_Light)
        glClearColor((float)235 / 255, (float)235 / 255, (float)235 / 255, 1.0f);
    else
        glClearColor((float)45 / 255, (float)45 / 255, (float)45 / 255, 1.0f);

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

            DrawString("melonDS " MELONDS_VERSION, 72, 30, 42, false, false);
            DrawLine(30, 88, 1250, 88, false);
            DrawLine(30, 648, 1250, 648, false);
            DrawLine(90, 124, 1190, 124, true);
            DrawString("ƒ Exit     ‚ Options      Back     € OK", 1218, 667, 34, false, true);

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

    if (Config::ScreenLayout == 0)
        Config::ScreenLayout = (Config::ScreenRotation % 2 == 0) ? 1 : 2;

    if (Config::ScreenLayout == 1)
    {
        if (Config::ScreenSizing == 0)
        {
            height_top = height_bot = 360 - gap / 2;
            if (Config::ScreenRotation % 2 == 0)
                width_top = width_bot = height_top * 4 / 3;
            else
                width_top = width_bot = height_top * 3 / 4;
        }
        else if (Config::ScreenSizing == 1)
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
        else
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
    else
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

        if (Config::ScreenSizing == 1)
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
        else if (Config::ScreenSizing == 2)
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
        memcpy(Framebuffer, GPU::Framebuffer, 256 * 384 * 4);

        chrono::duration<double> elapsed = chrono::steady_clock::now() - start;
        if (Config::LimitFPS && elapsed.count() < (float)1 / 60)
            usleep(((float)1 / 60 - elapsed.count()) * 1000000);
    }
}

void FillAudioBuffer()
{
    // 1440 samples seems to be the sweet spot for audout
    // which is 984 samples at the original sample rate

    s16 buf_in[984 * 2];
    s16 *buf_out = (s16*)OutBufferData;

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

void PlayAudio(void *args)
{
    while (!Paused)
    {
        FillAudioBuffer();
        audoutPlayBuffer(&AudOutBuffer, &ReleasedOutBuffer);
    }
}

void MicInput(void *args)
{
    while (!Paused)
    {
        audinCaptureBuffer(&AudInBuffer, &ReleasedInBuffer);
        if (Config::MicInputType == 0)
        {
            NDS::MicInputFrame(NULL, 0);
        }
        else if (Config::MicInputType == 1)
        {
            NDS::MicInputFrame((s16*)InBufferData, 1440);
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

    int clockspeeds[] = { 1020000000, 1224000000, 1581000000, 1785000000 };
    pcvSetClockRate(PcvModule_Cpu, clockspeeds[Config::SwitchOverclock]);

    if (!resume)
    {
        NDS::Init();
        NDS::LoadROM(ROMPath.c_str(), SRAMPath.c_str(), Config::DirectBoot);
    }

    SetScreenLayout();

    Paused = false;

    threadCreate(&Core, RunCore, NULL, 0x80000, 0x30, 1);
    threadStart(&Core);
    threadCreate(&Audio, PlayAudio, NULL, 0x80000, 0x30, 0);
    threadStart(&Audio);
    threadCreate(&Mic, MicInput, NULL, 0x80000, 0x2F, 0);
    threadStart(&Mic);
}

void PauseMenu()
{
    Paused = true;
    appletUnlockExit();

    if (MenuTheme == ColorSetId_Light)
        glClearColor((float)235 / 255, (float)235 / 255, (float)235 / 255, 1.0f);
    else
        glClearColor((float)45 / 255, (float)45 / 255, (float)45 / 255, 1.0f);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    vector<const char*> items = 
    {
        "Resume",
        "Close lid",
        "Save state",
        "Load state",
        "Options",
        "File browser"
    };
    if (LidClosed)
        items[1] = "Open lid";

    while (Paused)
    {
        unsigned int selection = 0;

        while (true)
        {
            glClear(GL_COLOR_BUFFER_BIT);
            DrawString("melonDS " MELONDS_VERSION, 72, 30, 42, false, false);
            DrawLine(30, 88, 1250, 88, false);
            DrawLine(30, 648, 1250, 648, false);
            DrawLine(90, 124, 1190, 124, true);
            DrawString("€ OK", 1218, 667, 34, false, true);
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

        if (selection == 0)
        {
            StartCore(true);
        }
        else if (selection == 1)
        {
            LidClosed = !LidClosed;
            NDS::SetLidClosed(LidClosed);
            StartCore(true);
        }
        else if (selection == 2 || selection == 3)
        {
            Savestate* state = new Savestate(const_cast<char*>(StatePath.c_str()), selection == 1);
            if (!state->Error)
            {
                NDS::DoSavestate(state);
                if (Config::SavestateRelocSRAM)
                    NDS::RelocateSave(const_cast<char*>(StateSRAMPath.c_str()), selection == 1);
            }
            delete state;

            StartCore(true);
        }
        else if (selection == 4)
        {
            OptionsMenu();
        }
        else if (selection == 5)
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
        DrawString("melonDS " MELONDS_VERSION, 72, 30, 42, false, false);
        DrawLine(30, 88, 1250, 88, false);
        DrawLine(30, 648, 1250, 648, false);
        DrawString("ƒ Exit", 1218, 667, 34, false, true);
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

    int datasize = 1440 * 2 * 2;
    int buffersize = (datasize + 0xfff) & ~0xfff;

    audoutInitialize();
    audoutStartAudioOut();

    OutBufferData = new u8[buffersize];
    AudOutBuffer.next = NULL;
    AudOutBuffer.buffer = OutBufferData;
    AudOutBuffer.buffer_size = buffersize;
    AudOutBuffer.data_size = datasize;
    AudOutBuffer.data_offset = 0;

    audinInitialize();
    audinStartAudioIn();

    InBufferData = new u8[buffersize];
    AudInBuffer.next = NULL;
    AudInBuffer.buffer = InBufferData;
    AudInBuffer.buffer_size = buffersize;
    AudInBuffer.data_size = datasize;
    AudInBuffer.data_offset = 0;

    pcvInitialize();

    StartCore(false);

    Framebuffer = new u32[256 * 384];

    HidControllerKeys keys[] = { KEY_A, KEY_B, KEY_MINUS, KEY_PLUS, KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_ZR, KEY_ZL, KEY_X, KEY_Y };

    while (appletMainLoop())
    {
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, Framebuffer);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, &Framebuffer[256 * 192]);
        glDrawArrays(GL_TRIANGLE_FAN, 4, 4);
        eglSwapBuffers(Display, Surface);
    }

    Paused = true;
    pcvSetClockRate(PcvModule_Cpu, 1020000000);
    pcvExit();
    audinExit();
    audoutExit();
    DeInitRenderer();
    appletUnlockExit();
    return 0;
}

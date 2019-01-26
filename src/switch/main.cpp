/*
    Copyright 2018-2019 Hydr8gon

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
u8 *Font, *FontColor, *Folder;

char *EmuDirectory;
string ROMPath, SRAMPath, StatePath, StateSRAMPath;
vector<string> Files;

int SamplesOut;
s16 *AudOutBufferData, *AudInBufferData;
AudioOutBuffer AudOutBuffer, *RelOutBuffer;
AudioInBuffer AudInBuffer, *RelInBuffer;

u32 *DisplayBuffer;
unsigned int TouchBoundLeft, TouchBoundRight, TouchBoundTop, TouchBoundBottom;

EGLDisplay Display;
EGLContext Context;
EGLSurface Surface;
GLuint Program, VertArrayObj, VertBufferObj, Texture;

u32 HotkeyMask;
bool LidClosed;

AppletHookCookie HookCookie;

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
    vector<string> entries;
    int *value;
} Setting;

const vector<string> ControlEntries =
{
    "Default",
    "A Button", "B Button", "X Button", "Y Button",
    "Left Stick Click", "Right Stick Click",
    "L Button", "R Button", "ZL Button", "ZR Button",
    "Plus Button", "Minus Button",
    "D-Pad Left", "D-Pad Up", "D-Pad Right", "D-Pad Down",
    "Left Stick Left", "Left Stick Up", "Left Stick Right", "Left Stick Down",
    "Right Stick Left", "Right Stick Up", "Right Stick Right", "Right Stick Down"
};

const vector<Setting> Controls =
{
    { "A Button",          ControlEntries, &Config::JoyMapping[0]         },
    { "B Button",          ControlEntries, &Config::JoyMapping[1]         },
    { "Select Button",     ControlEntries, &Config::JoyMapping[2]         },
    { "Start Button",      ControlEntries, &Config::JoyMapping[3]         },
    { "D-Pad Right",       ControlEntries, &Config::JoyMapping[4]         },
    { "D-Pad Left",        ControlEntries, &Config::JoyMapping[5]         },
    { "D-Pad Up",          ControlEntries, &Config::JoyMapping[6]         },
    { "D-Pad Down",        ControlEntries, &Config::JoyMapping[7]         },
    { "R Button",          ControlEntries, &Config::JoyMapping[8]         },
    { "L Button",          ControlEntries, &Config::JoyMapping[9]         },
    { "X Button",          ControlEntries, &Config::JoyMapping[10]        },
    { "Y Button",          ControlEntries, &Config::JoyMapping[11]        },
    { "Close/Open Lid",    ControlEntries, &Config::HKJoyMapping[HK_Lid]  },
    { "Microphone",        ControlEntries, &Config::HKJoyMapping[HK_Mic]  },
    { "Pause Menu",        ControlEntries, &Config::HKJoyMapping[HK_Menu] },
    { "Reset to Defaults", {},             NULL                           }
};

const vector<Setting> Settings =
{
    { "Boot Game Directly",                 { "Off", "On" },                                                               &Config::DirectBoot },
    { "Threaded 3D Renderer",               { "Off", "On" },                                                               &Config::Threaded3D },
    { "Audio Volume",                       { "0%", "25%", "50%", "75%", "100%" },                                         &Config::AudioVolume },
    { "Microphone Input",                   { "None", "Microphone", "White Noise" },                                       &Config::MicInputType },
    { "Separate Savefiles from Savestates", { "Off", "On" },                                                               &Config::SavestateRelocSRAM },
    { "Screen Rotation",                    { "0", "90", "180", "270" },                                                   &Config::ScreenRotation },
    { "Mid-Screen Gap",                     { "0 Pixels", "1 Pixel", "8 Pixels", "64 Pixels", "90 Pixels", "128 Pixels" }, &Config::ScreenGap },
    { "Screen Layout",                      { "Natural", "Vertical", "Horizontal" },                                       &Config::ScreenLayout },
    { "Screen Sizing",                      { "Even", "Emphasize Top", "Emphasize Bottom" },                               &Config::ScreenSizing },
    { "Screen Filtering",                   { "Off", "On" },                                                               &Config::ScreenFilter },
    { "Limit Framerate",                    { "Off", "On" },                                                               &Config::LimitFPS },
    { "Switch Overclock",                   { "1020 MHz", "1224 MHz", "1581 MHz", "1785 MHz" },                            &Config::SwitchOverclock }
};

const vector<string> PauseMenuItems =
{
    "Resume",
    "Save State",
    "Load State",
    "Settings",
    "File Browser"
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

u8 *TexFromBMP(string filename)
{
    FILE *bmp = melon_fopen(filename.c_str(), "rb");

    // Deal with the file header
    u8 header[54];
    fread(header, sizeof(u8), 54, bmp);
    int width = *(int*)&header[18];
    int height = *(int*)&header[22];

    // The bitmap data is stored from bottom to top, so reverse it
    u8 *tex = new u8[width * height * 3];
    for (int i = 1; i <= height; i++)
        fread(&tex[width * (height - i) * 3], sizeof(u8), width * 3, bmp);

    fclose(bmp);
    return tex;
}

u8 *IconFromROM(string filename)
{
    FILE *rom = melon_fopen(filename.c_str(), "rb");

    u32 offset;
    fseek(rom, 0x68, SEEK_SET);
    fread(&offset, sizeof(u32), 1, rom);

    u8 data[512];
    fseek(rom, 0x20 + offset, SEEK_SET);
    fread(data, sizeof(u8), 512, rom);

    u16 palette[16];
    fseek(rom, 0x220 + offset, SEEK_SET);
    fread(palette, sizeof(u16), 16, rom);

    fclose(rom);

    // Get the 4-bit palette indexes
    u8 indexes[1024];
    for (int i = 0; i < 512; i++)
    {
        indexes[i * 2] = data[i] << 4;
        indexes[i * 2 + 1] = data[i];
    }

    // Get each pixel's 5-bit palette color and convert it to 8-bit
    u8 tiles[32 * 32 * 3];
    for (int i = 0; i < 1024; i++)
    {
        tiles[i * 3] = ((palette[indexes[i] / 16] >> 10) & 0x1f) * 255 / 31;
        tiles[i * 3 + 1] = ((palette[indexes[i] / 16] >> 5) & 0x1f) * 255 / 31;
        tiles[i * 3 + 2] = (palette[indexes[i] / 16] & 0x1f) * 255 / 31;
    }

    // Rearrange the Pixels from 8x8 tiles to a 32x32 texture
    u8 *tex = new u8[32 * 32 * 3];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 4; k++)
                memcpy(&tex[(256 * i + 32 * j + 8 * k) * 3], &tiles[(256 * i + 8 * j + 64 * k) * 3], 8 * 3);

    return tex;
}

void DrawString(string str, float x, float y, int size, bool color, bool fromright)
{
    int width = 0;
    for (unsigned int i = 0; i < str.size(); i++)
        width += CharWidth[str[i] - 32];

    u8 *font = color ? FontColor : Font;
    u8 *tex = new u8[width * 48 * 3];
    int currentx = 0;

    for (unsigned int i = 0; i < str.size(); i++)
    {
        int col = str[i] - 32;
        int row = 0;
        while (col > 9)
        {
            col -= 10;
            row++;
        }

        for (int j = 0; j < 48; j++)
            memcpy(&tex[(j * width + currentx) * 3], &font[((row * 512 + col) * 48 + j * 512) * 3], CharWidth[str[i] - 32] * 3);

        currentx += CharWidth[str[i] - 32];
    }

    if (fromright)
        x -= width * size / 48;

    Vertex string[] =
    {
        { { x + width * size / 48, y + size }, { 1.0f, 1.0f } },
        { { x,                     y + size }, { 0.0f, 1.0f } },
        { { x,                     y        }, { 0.0f, 0.0f } },
        { { x + width * size / 48, y        }, { 1.0f, 0.0f } }
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

    u8 tex[3];
    if (MenuTheme == ColorSetId_Light)
        memset(tex, color ? 205 : 45, sizeof(tex));
    else
        memset(tex, color ? 77 : 255, sizeof(tex));

    glBufferData(GL_ARRAY_BUFFER, sizeof(line), line, GL_DYNAMIC_DRAW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_BGR, GL_UNSIGNED_BYTE, tex);
    glDrawArrays(GL_LINES, 0, 2);
}

void DrawIcon(u8 *tex, float x, float y, int size)
{
    Vertex icon[] =
    {
        { { x + 64, y + 64 }, { 1.0f, 1.0f } },
        { { x,      y + 64 }, { 0.0f, 1.0f } },
        { { x,      y      }, { 0.0f, 0.0f } },
        { { x + 64, y      }, { 1.0f, 0.0f } }
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof(icon), icon, GL_DYNAMIC_DRAW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, size, size, 0, GL_BGR, GL_UNSIGNED_BYTE, tex);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void Menu(string title, string Buttons, int rowcount, bool (*Buttonactions)(u32, int), void (*drawrow)(int, int, int))
{
    float clearcolor = ((MenuTheme == ColorSetId_Light) ? 235.0f : 45.0f) / 255;
    glClearColor(clearcolor, clearcolor, clearcolor, 1.0f);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    int selection = 0;
    bool upheld = false;
    bool downheld = false;
    bool scroll = false;
    chrono::steady_clock::time_point timeheld;

    while (true)
    {
        glClear(GL_COLOR_BUFFER_BIT);
        DrawString(title, 72, 30, 42, false, false);
        DrawLine(30, 88, 1250, 88, false);
        DrawLine(30, 648, 1250, 648, false);
        DrawLine(90, 124, 1190, 124, true);
        DrawString(Buttons, 1218, 667, 34, false, true);

        hidScanInput();
        u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
        u32 released = hidKeysUp(CONTROLLER_P1_AUTO);

        if (pressed & KEY_UP && selection > 0)
        {
            selection--;
            upheld = true;
            timeheld = chrono::steady_clock::now();
        }
        else if (pressed & KEY_DOWN && selection < rowcount - 1)
        {
            selection++;
            downheld = true;
            timeheld = chrono::steady_clock::now();
        }
        if (Buttonactions(pressed, selection))
            break;

        if (released & KEY_UP)
        {
            upheld = false;
            scroll = false;
        }
        if (released & KEY_DOWN)
        {
            downheld = false;
            scroll = false;
        }

        if ((upheld && selection > 0) || (downheld && selection < rowcount - 1))
        {
            chrono::duration<double> elapsed = chrono::steady_clock::now() - timeheld;
            if (!scroll && elapsed.count() > 0.5f)
                scroll = true;
            if (scroll && elapsed.count() > 0.1f)
            {
                selection += (upheld && selection > 0) ? -1 : 1;
                timeheld = chrono::steady_clock::now();
            }
        }

        for (int i = 0; i < 7; i++)
        {
            if (i < rowcount)
            {
                int row;
                if (selection < 4 || rowcount <= 7)
                    row = i;
                else if (selection > rowcount - 4)
                    row = rowcount - 7 + i;
                else
                   row = i + selection - 3;

                drawrow(i, row, selection);
                DrawLine(90, 194 + i * 70, 1190, 194 + i * 70, true);
            }
        }

        eglSwapBuffers(Display, Surface);
    }
}

bool ControlsButtonActions(u32 pressed, int selection)
{
    if (pressed & KEY_A)
    {
        if (selection == 15) // Reset to defaults
        {
            for (unsigned int i = 0; i < Controls.size() - 1; i++)
                *Controls[i].value = -1;
        }
        else
        {
            pressed = 0;
            while (!pressed)
            {
                hidScanInput();
                pressed = hidKeysDown(CONTROLLER_P1_AUTO);
            }

            for (unsigned int i = 0; i < ControlEntries.size(); i++)
            {
                if (pressed & BIT(i))
                    *Controls[selection].value = i;
            }
        }
    }
    else if (pressed & KEY_B)
    {
        return true;
    }

    return false;
}

void ControlsDrawRow(int i, int row, int selection)
{
    DrawString(Controls[row].name, 105, 140 + i * 70, 38, row == selection, false);
    if (row != 15) // Reset to defaults
        DrawString(Controls[row].entries[*Controls[row].value + 1], 1175, 143 + i * 70, 32, row == selection, true);
}

void ControlsMenu()
{
    Menu("Controls", " Back     € OK", Controls.size(), ControlsButtonActions, ControlsDrawRow);
}

bool SettingsButtonActions(u32 pressed, int selection)
{
    if (pressed & KEY_A)
    {
        if (selection == 2) // Audio volume
        {
            (*Settings[selection].value) += 256 / 4;
            if (*Settings[selection].value > 256)
                *Settings[selection].value = 0;
        }
        else
        {
            (*Settings[selection].value)++;
            if (*Settings[selection].value >= (int)Settings[selection].entries.size())
                *Settings[selection].value = 0;
        }
    }
    else if (pressed & KEY_B)
    {
        Config::Save();
        return true;
    }
    else if (pressed & KEY_X)
    {
        ControlsMenu();
    }

    return false;
}

void SettingsDrawRow(int i, int row, int selection)
{
    DrawString(Settings[row].name, 105, 140 + i * 70, 38, row == selection, false);
    if (row == 2) // Audio volume
        DrawString(Settings[row].entries[*Settings[row].value * 4 / 256], 1175, 143 + i * 70, 32, row == selection, true);
    else
        DrawString(Settings[row].entries[*Settings[row].value], 1175, 143 + i * 70, 32, row == selection, true);
}

void SettingsMenu()
{
    Menu("Settings", "‚ Controls      Back     € OK", Settings.size(), SettingsButtonActions, SettingsDrawRow);
}

bool FilesButtonActions(u32 pressed, int selection)
{
    if (pressed & KEY_A && Files.size() > 0)
    {
        ROMPath += "/" + Files[selection];
        selection = 0;
        return true;
    }
    else if (pressed & KEY_B && ROMPath != "sdmc:/")
    {
        ROMPath = ROMPath.substr(0, ROMPath.rfind("/"));
        selection = 0;
        return true;
    }
    else if (pressed & KEY_X)
    {
        SettingsMenu();
    }
    else if (pressed & KEY_PLUS)
    {
        ROMPath = "";
        return true;
    }

    return false;
}

void FilesDrawRow(int i, int row, int selection)
{
    DrawString(Files[row], 184, 140 + i * 70, 38, row == selection, false);
    if (Files[row].find(".nds", Files[row].length() - 4) == string::npos)
    {
        DrawIcon(Folder, 105, 127 + i * 70, 64);
    }
    else
    {
        u8 *icon = IconFromROM(ROMPath + "/" + Files[row]);
        DrawIcon(icon, 105, 126 + i * 70, 32);
        delete[] icon;
    }
}

void FilesMenu()
{
    ROMPath = (strcmp(Config::LastROMFolder, "") == 0) ? "sdmc:/" : Config::LastROMFolder;

    while (ROMPath.find(".nds", ROMPath.length() - 4) == string::npos)
    {
        DIR *dir = opendir(ROMPath.c_str());
        dirent *entry;
        while ((entry = readdir(dir)))
        {
            string name = entry->d_name;
            if (entry->d_type == DT_DIR || name.find(".nds", (name.length() - 4)) != string::npos)
                Files.push_back(name);
        }
        closedir(dir);
        sort(Files.begin(), Files.end());

        Menu("melonDS " MELONDS_VERSION, "ƒ Exit     ‚ Settings      Back     € OK", Files.size(), FilesButtonActions, FilesDrawRow);
        if (ROMPath == "")
            return;

        Files.clear();
    }

    string folder = ROMPath.substr(0, ROMPath.rfind("/")).c_str();
    folder.append(1, '\0');
    strncpy(Config::LastROMFolder, folder.c_str(), folder.length());
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
    while (!(HotkeyMask & BIT(HK_Menu)))
    {
        chrono::steady_clock::time_point start = chrono::steady_clock::now();

        NDS::RunFrame();
        memcpy(DisplayBuffer, GPU::Framebuffer, sizeof(GPU::Framebuffer));

        chrono::duration<double> elapsed = chrono::steady_clock::now() - start;
        if (Config::LimitFPS && elapsed.count() < 1.0f / 60)
            usleep((1.0f / 60 - elapsed.count()) * 1000000);
    }
}

void SetupAudioBuffer()
{
    // Dynamically switch audio sample rate when the system is docked/undocked
    // For some reason both modes act differently with different sample rates
    SamplesOut = (appletGetOperationMode() == AppletOperationMode_Handheld) ? 1440 : 2048;
    AudOutBufferData = new s16[(SamplesOut * 2 + 0xfff) & ~0xfff];
    AudOutBuffer.next = NULL;
    AudOutBuffer.buffer = AudOutBufferData;
    AudOutBuffer.buffer_size = (SamplesOut * sizeof(s16) * 2 + 0xfff) & ~0xfff;
    AudOutBuffer.data_size = SamplesOut * sizeof(s16) * 2;
    AudOutBuffer.data_offset = 0;
}

void FillAudioBuffer()
{
    // Approximate the equivalent sample count at the original rate
    int samples_in = SamplesOut * 700 / 1024;

    s16 buf_in[samples_in * 2];
    s16 *buf_out = AudOutBufferData;

    int num_in = SPU::ReadOutput(buf_in, samples_in);
    int num_out = SamplesOut;

    int margin = 6;
    if (num_in < samples_in - margin)
    {
        int last = num_in - 1;
        if (last < 0)
            last = 0;

        for (int i = num_in; i < samples_in - margin; i++)
            ((u32*)buf_in)[i] = ((u32*)buf_in)[last];

        num_in = samples_in - margin;
    }

    float res_incr = (float)num_in / num_out;
    float res_timer = 0;
    int res_pos = 0;

    for (int i = 0; i < SamplesOut; i++)
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

void AudioOutput(void *args)
{
    while (!(HotkeyMask & BIT(HK_Menu)))
    {
        FillAudioBuffer();
        audoutPlayBuffer(&AudOutBuffer, &RelOutBuffer);
    }
}

void MicInput(void *args)
{
    while (!(HotkeyMask & BIT(HK_Menu)))
    {
        if (Config::MicInputType == 0 || !(HotkeyMask & BIT(HK_Mic)))
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

void AppletHook(AppletHookType hook, void *param)
{
    if (hook == AppletHookType_OnOperationMode || hook == AppletHookType_OnPerformanceMode)
    {
        pcvSetClockRate(PcvModule_Cpu, ClockSpeeds[Config::SwitchOverclock]);
        SetupAudioBuffer();
    }
}

void StartCore(bool resume)
{
    SetupAudioBuffer();
    SetScreenLayout();

    appletLockExit();
    appletHook(&HookCookie, AppletHook, NULL);
    if (Config::AudioVolume > 0)
    {
        audoutInitialize();
        audoutStartAudioOut();
    }
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
    HotkeyMask &= ~BIT(HK_Menu);

    if (!resume)
    {
        SRAMPath = ROMPath.substr(0, ROMPath.rfind(".")) + ".sav";
        StatePath = ROMPath.substr(0, ROMPath.rfind(".")) + ".mln";
        StateSRAMPath = StatePath + ".sav";

        NDS::Init();
        NDS::LoadROM(ROMPath.c_str(), SRAMPath.c_str(), Config::DirectBoot);
    }

    Thread core, audio, mic;
    threadCreate(&core, RunCore, NULL, 0x80000, 0x30, 1);
    threadStart(&core);
    threadCreate(&audio, AudioOutput, NULL, 0x80000, 0x2F, 0);
    threadStart(&audio);
    threadCreate(&mic, MicInput, NULL, 0x80000, 0x30, 0);
    threadStart(&mic);
}

void Pause()
{
    HotkeyMask |= BIT(HK_Menu);
    pcvSetClockRate(PcvModule_Cpu, ClockSpeeds[0]);
    pcvExit();
    audinStopAudioIn();
    audinExit();
    audoutStopAudioOut();
    audoutExit();
    appletUnhook(&HookCookie);
    appletUnlockExit();
}

bool PauseButtonActions(u32 pressed, int selection)
{
    if (pressed & KEY_A)
    {
        if (selection == 0) // Resume
        {
            StartCore(true);
            return true;
        }
        else if (selection == 1 || selection == 2) // Save/load state
        {
            Savestate *state = new Savestate(const_cast<char*>(StatePath.c_str()), selection == 2);
            if (!state->Error)
            {
                NDS::DoSavestate(state);
                if (Config::SavestateRelocSRAM)
                    NDS::RelocateSave(const_cast<char*>(StateSRAMPath.c_str()), selection == 2);
            }
            delete state;

            StartCore(true);
            return true;
        }
        else if (selection == 3) // Settings
        {
            SettingsMenu();
        }
        else // File browser
        {
            NDS::DeInit();
            FilesMenu();
            if (ROMPath != "")
                StartCore(false);
            return true;
        }
    }

    return false;
}

void PauseDrawRow(int i, int row, int selection)
{
    DrawString(PauseMenuItems[row], 105, 140 + i * 70, 38, row == selection, false);
}

void PauseMenu()
{
    Pause();
    Menu("melonDS " MELONDS_VERSION, "€ OK", PauseMenuItems.size(), PauseButtonActions, PauseDrawRow);
}

bool LocalFileExists(const char *name)
{
    FILE *file = melon_fopen_local(name, "rb");
    if (!file)
        return false;
    fclose(file);
    return true;
}

int main(int argc, char **argv)
{
    InitRenderer();

    setsysInitialize();
    setsysGetColorSetId(&MenuTheme);
    setsysExit();

    romfsInit();
    string theme = (MenuTheme == ColorSetId_Light) ? "light" : "dark";
    Font = TexFromBMP("romfs:/font-" + theme + ".bmp");
    FontColor = TexFromBMP("romfs:/fontcolor-" + theme + ".bmp");
    Folder = TexFromBMP("romfs:/folder-" + theme + ".bmp");
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

    AudInBufferData = new s16[(1440 * 2 + 0xfff) & ~0xfff];
    AudInBuffer.next = NULL;
    AudInBuffer.buffer = AudInBufferData;
    AudInBuffer.buffer_size = (1440 * 2 * sizeof(s16) + 0xfff) & ~0xfff;
    AudInBuffer.data_size = 1440 * 2 * sizeof(s16);
    AudInBuffer.data_offset = 0;

    StartCore(false);

    DisplayBuffer = new u32[256 * 384];

    u32 defaultkeys[] =
    {
        KEY_A, KEY_B, KEY_MINUS, KEY_PLUS,
        KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN,
        KEY_ZR, KEY_ZL, KEY_X, KEY_Y,
        KEY_RSTICK, KEY_LSTICK, (KEY_L | KEY_R)
    };

    while (appletMainLoop())
    {
        hidScanInput();
        u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
        u32 released = hidKeysUp(CONTROLLER_P1_AUTO);

        for (int i = 0; i < 12; i++)
        {
            u32 key = ((Config::JoyMapping[i] == -1) ? defaultkeys[i] : BIT(Config::JoyMapping[i]));
            if (pressed & key)
                NDS::PressKey(i > 9 ? i + 6 : i);
            if (released & key)
                NDS::ReleaseKey(i > 9 ? i + 6 : i);
        }

        for (int i = 0; i < HK_MAX; i++)
        {
            u32 key = ((Config::HKJoyMapping[i] == -1) ? defaultkeys[i + 12] : BIT(Config::HKJoyMapping[i]));
            if (pressed & key)
                HotkeyMask |= BIT(i);
            else if (released & key)
                HotkeyMask &= ~BIT(i);
        }

        if (HotkeyMask & BIT(HK_Lid))
        {
            LidClosed = !LidClosed;
            NDS::SetLidClosed(LidClosed);
            HotkeyMask &= ~BIT(HK_Lid);
        }

        if (HotkeyMask & BIT(HK_Menu))
        {
            PauseMenu();
            if (ROMPath == "")
                break;
        }

        if (hidTouchCount() > 0)
        {
            touchPosition touch;
            hidTouchRead(&touch, 0);

            if (touch.px > TouchBoundLeft && touch.px < TouchBoundRight && touch.py > TouchBoundTop && touch.py < TouchBoundBottom)
            {
                int x, y;
                if (Config::ScreenRotation == 0) // 0
                {
                    x = (touch.px - TouchBoundLeft) * 256.0f / (TouchBoundRight - TouchBoundLeft);
                    y = (touch.py - TouchBoundTop) * 256.0f / (TouchBoundRight - TouchBoundLeft);
                }
                else if (Config::ScreenRotation == 1) // 90
                {
                    x = (touch.py - TouchBoundTop) * 192.0f / (TouchBoundRight - TouchBoundLeft);
                    y = 192 - (touch.px - TouchBoundLeft) * 192.0f / (TouchBoundRight - TouchBoundLeft);
                }
                else if (Config::ScreenRotation == 2) // 180
                {
                    x = (touch.px - TouchBoundLeft) * -256.0f / (TouchBoundRight - TouchBoundLeft);
                    y = 192 - (touch.py - TouchBoundTop) * 256.0f / (TouchBoundRight - TouchBoundLeft);
                }
                else // 270
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
    return 0;
}

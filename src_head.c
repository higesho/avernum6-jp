// Avernum 6 - Japanese text support
// (ported from the Avernum: Escape From the Pit / Avernum 2-5 JP mods)
//
// Built as a proxy dsound.dll (DirectSoundCreate is passed through to the system dsound.dll).
// Avernum 6 draws text with its own bitmap fonts through OpenGL texture objects. The DLL
//   * replaces the routine that lays out and draws text, so that EUC-JP characters are measured
//     with GDI and drawn from glyph textures of its own (ASCII stays with the game's fonts);
//   * fixes the script loader's token estimate, which Japanese literals would overflow;
//   * redirects exe strings (.rdata) and the data files (scripts, scenario, pictures)
//     to the translated versions, as in the Avernum 3 mod.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <GL/gl.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "opengl32.lib")

static BYTE *g_base;
#define VA(x) ((void *)(g_base + ((DWORD)(x) - 0x400000)))
#define GAME_CP 20932 /* EUC-JP */
static int g_eucjp = 1; // used by to_game_encoding(): game text is EUC-JP


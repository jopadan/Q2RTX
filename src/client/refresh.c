/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

// Main windowed and fullscreen graphics interface module. This module
// is used for both the software and OpenGL rendering versions of the
// Quake refresh engine.


#include "client.h"
#include "refresh/debug.h"
#include "refresh/images.h"
#include "refresh/models.h"
#include "shared/debug.h"

// Console variables that we need to access from this module
cvar_t      *vid_rtx;
cvar_t      *vid_geometry;
cvar_t      *vid_modelist;
cvar_t      *vid_fullscreen;
cvar_t      *_vid_fullscreen;
cvar_t      *vid_display;
cvar_t      *vid_displaylist;

// used in gl and vkpt renderers
int registration_sequence;

vid_driver_t    vid;

#define MODE_GEOMETRY   1
#define MODE_FULLSCREEN 2
#define MODE_MODELIST   4

static int  mode_changed;

/*
==========================================================================

HELPER FUNCTIONS

==========================================================================
*/

// 640x480 800x600 1024x768
// 640x480@75
// 640x480@75:32
// 640x480:32@75
bool VID_GetFullscreen(vrect_t *rc, int *freq_p, int *depth_p)
{
    unsigned long w, h, freq, depth;
    char *s;
    int mode;

    // fill in default parameters
    rc->x = 0;
    rc->y = 0;
    rc->width = 640;
    rc->height = 480;

    if (freq_p)
        *freq_p = 0;
    if (depth_p)
        *depth_p = 0;

    if (!vid_modelist || !vid_fullscreen)
        return false;

    s = vid_modelist->string;
    while (Q_isspace(*s))
        s++;
    if (!*s)
        return false;

    mode = 1;
    while (1) {
        if (!strncmp(s, "desktop", 7)) {
            s += 7;
            if (*s && !Q_isspace(*s)) {
                Com_DPrintf("Mode %d is malformed\n", mode);
                return false;
            }
            w = h = freq = depth = 0;
        } else {
            w = strtoul(s, &s, 10);
            if (*s != 'x' && *s != 'X') {
                Com_DPrintf("Mode %d is malformed\n", mode);
                return false;
            }
            h = strtoul(s + 1, &s, 10);
            freq = depth = 0;
            if (*s == '@') {
                freq = strtoul(s + 1, &s, 10);
                if (*s == ':') {
                    depth = strtoul(s + 1, &s, 10);
                }
            } else if (*s == ':') {
                depth = strtoul(s + 1, &s, 10);
                if (*s == '@') {
                    freq = strtoul(s + 1, &s, 10);
                }
            }
        }
        if (mode == vid_fullscreen->integer) {
            break;
        }
        while (Q_isspace(*s))
            s++;
        if (!*s) {
            Com_DPrintf("Mode %d not found\n", vid_fullscreen->integer);
            return false;
        }
        mode++;
    }

    // sanity check
    if (w < 320 || w > 8192 || h < 240 || h > 8192 || freq > 1000 || depth > 32) {
        Com_DPrintf("Mode %lux%lu@%lu:%lu doesn't look sane\n", w, h, freq, depth);
        return false;
    }

    rc->width = w;
    rc->height = h;

    if (freq_p)
        *freq_p = freq;
    if (depth_p)
        *depth_p = depth;

    return true;
}

// 640x480
// 640x480+0
// 640x480+0+0
// 640x480-100-100
bool VID_GetGeometry(vrect_t *rc)
{
    unsigned long w, h;
    long x, y;
    char *s;

    // fill in default parameters
    rc->x = 100;
    rc->y = 100;
    rc->width = 1280;
    rc->height = 720;

    if (!vid_geometry)
        return false;

    s = vid_geometry->string;
    if (!*s)
        return false;

    w = strtoul(s, &s, 10);
    if (*s != 'x' && *s != 'X') {
        Com_DPrintf("Geometry string is malformed\n");
        return false;
    }
    h = strtoul(s + 1, &s, 10);
	x = rc->x;
	y = rc->y;
    if (*s == '+' || *s == '-') {
        x = strtol(s, &s, 10);
        if (*s == '+' || *s == '-') {
            y = strtol(s, &s, 10);
        }
    }

    // sanity check
    if (w < 320 || w > 8192 || h < 240 || h > 8192) {
        Com_DPrintf("Geometry %lux%lu doesn't look sane\n", w, h);
        return false;
    }

    rc->x = x;
    rc->y = y;
    rc->width = w;
    rc->height = h;

    return true;
}

void VID_SetGeometry(vrect_t *rc)
{
    char buffer[MAX_QPATH];

    if (!vid_geometry)
        return;

    Q_snprintf(buffer, sizeof(buffer), "%dx%d%+d%+d",
               rc->width, rc->height, rc->x, rc->y);
    Cvar_SetByVar(vid_geometry, buffer, FROM_CODE);
}

void VID_ToggleFullscreen(void)
{
    if (!vid_fullscreen || !_vid_fullscreen)
        return;

    if (!vid_fullscreen->integer) {
        if (!_vid_fullscreen->integer) {
            Cvar_Set("_vid_fullscreen", "1");
        }
        Cbuf_AddText(&cmd_buffer, "set vid_fullscreen $_vid_fullscreen\n");
    } else {
        Cbuf_AddText(&cmd_buffer, "set vid_fullscreen 0\n");
    }
}

/*
==========================================================================

LOADING / SHUTDOWN

==========================================================================
*/

extern const vid_driver_t   vid_sdl;

static const vid_driver_t *const vid_drivers[] = {
    &vid_sdl,
    NULL
};

/*
============
CL_RunResfresh
============
*/
void CL_RunRefresh(void)
{
    if (!cls.ref_initialized) {
        return;
    }

    vid.pump_events();

    if (mode_changed) {
        if (mode_changed & MODE_FULLSCREEN) {
            vid.set_mode();
            if (vid_fullscreen->integer) {
                Cvar_Set("_vid_fullscreen", vid_fullscreen->string);
            }
        } else {
            if (vid_fullscreen->integer) {
                if (mode_changed & MODE_MODELIST) {
                    vid.set_mode();
                }
            } else {
                if (mode_changed & MODE_GEOMETRY) {
                    vid.set_mode();
                }
            }
        }
        mode_changed = 0;
    }

    if (cvar_modified & CVAR_REFRESH) {
        CL_RestartRefresh(true);
        cvar_modified &= ~CVAR_REFRESH;
    } else if (cvar_modified & CVAR_FILES) {
        CL_RestartRefresh(false);
        cvar_modified &= ~CVAR_FILES;
    }
}

static void vid_geometry_changed(cvar_t *self)
{
    mode_changed |= MODE_GEOMETRY;
}

static void vid_fullscreen_changed(cvar_t *self)
{
    mode_changed |= MODE_FULLSCREEN;
}

static void vid_modelist_changed(cvar_t *self)
{
    mode_changed |= MODE_MODELIST;
}

static void vid_driver_g(genctx_t *ctx)
{
    for (int i = 0; vid_drivers[i]; i++)
        Prompt_AddMatch(ctx, vid_drivers[i]->name);
}

/*
============
CL_InitRefresh
============
*/
void CL_InitRefresh(void)
{
    char *modelist;
    int i;

    if (cls.ref_initialized) {
        return;
    }

    vid_display = Cvar_Get("vid_display", "0", CVAR_ARCHIVE | CVAR_REFRESH);
    vid_displaylist = Cvar_Get("vid_displaylist", "\"<unknown>\" 0", CVAR_ROM);

    // Create the video variables so we know how to start the graphics drivers

	vid_rtx = Cvar_Get("vid_rtx", 
#if REF_VKPT
		"1",
#else
		"0",
#endif
		CVAR_REFRESH | CVAR_ARCHIVE);

    cvar_t *vid_driver = Cvar_Get("vid_driver", "", CVAR_REFRESH);
    vid_driver->generator = vid_driver_g;
    vid_fullscreen = Cvar_Get("vid_fullscreen", "0", CVAR_ARCHIVE);
    _vid_fullscreen = Cvar_Get("_vid_fullscreen", "1", CVAR_ARCHIVE);
    vid_geometry = Cvar_Get("vid_geometry", VID_GEOMETRY, CVAR_ARCHIVE);

    if (vid_fullscreen->integer) {
        Cvar_Set("_vid_fullscreen", vid_fullscreen->string);
    } else if (!_vid_fullscreen->integer) {
        Cvar_Set("_vid_fullscreen", "1");
    }

    Com_SetLastError("No available video driver");

#if REF_GL && REF_VKPT
	if (vid_rtx->integer)
		R_RegisterFunctionsRTX();
	else
		R_RegisterFunctionsGL();
#elif REF_GL
	R_RegisterFunctionsGL();
#elif REF_VKPT
	R_RegisterFunctionsRTX();
#else
#error "REF_GL and REF_VKPT are both disabled, at least one has to be enableds"
#endif

    // Try to initialize selected driver first
    ref_type_t ref_type = REF_TYPE_NONE;
    for (i = 0; vid_drivers[i]; i++) {
        if (!strcmp(vid_drivers[i]->name, vid_driver->string)) {
            vid = *vid_drivers[i];
            ref_type = R_Init(true);
            break;
        }
    }

    if (!vid_drivers[i] && vid_driver->string[0]) {
        Com_Printf("No such video driver: %s.\n"
                   "Available video drivers: ", vid_driver->string);
        for (int j = 0; vid_drivers[j]; j++) {
            if (j)
                Com_Printf(", ");
            Com_Printf("%s", vid_drivers[j]->name);
        }
        Com_Printf(".\n");
    }

    // Fall back to other available drivers
    if (ref_type == REF_TYPE_NONE) {
        int tried = i;
        for (i = 0; vid_drivers[i]; i++) {
            if (i == tried || !vid_drivers[i]->probe || !vid_drivers[i]->probe())
                continue;
            vid = *vid_drivers[i];
            if ((ref_type = R_Init(true)) != REF_TYPE_NONE)
                break;
        }
        Cvar_Reset(vid_driver);
    }

    if (ref_type == REF_TYPE_NONE)
        Com_Error(ERR_FATAL, "Couldn't initialize refresh: %s", Com_GetLastError());

    modelist = vid.get_mode_list();
    vid_modelist = Cvar_Get("vid_modelist", modelist, 0);
    Z_Free(modelist);

    vid.set_mode();

    cls.ref_type = ref_type;
    cls.ref_initialized = true;

    vid_geometry->changed = vid_geometry_changed;
    vid_fullscreen->changed = vid_fullscreen_changed;
    vid_modelist->changed = vid_modelist_changed;

    mode_changed = 0;

    FX_Init();

    // Initialize the rest of graphics subsystems
    V_Init();
    SCR_Init();
    UI_Init();

    R_ClearDebugLines();
    Cmd_AddCommand("cleardebuglines", R_ClearDebugLines);
    R_InitDebugText();

    SCR_RegisterMedia();
    Con_RegisterMedia();

    cvar_modified &= ~(CVAR_FILES | CVAR_REFRESH);
}

/*
============
CL_ShutdownRefresh
============
*/
void CL_ShutdownRefresh(void)
{
    if (!cls.ref_initialized) {
        return;
    }

    // Shutdown the rest of graphics subsystems
    V_Shutdown();
    SCR_Shutdown();
    UI_Shutdown();

    Cmd_RemoveCommand("cleardebuglines");

    vid_geometry->changed = NULL;
    vid_fullscreen->changed = NULL;
    vid_modelist->changed = NULL;

    R_Shutdown(true);

    memset(&vid, 0, sizeof(vid));

    cls.ref_initialized = false;
    cls.ref_type = REF_TYPE_NONE;

    // no longer active
    cls.active = ACT_MINIMIZED;

    Z_LeakTest(TAG_RENDERER);
}


refcfg_t r_config;

ref_type_t(*R_Init)(bool total) = NULL;
void(*R_Shutdown)(bool total) = NULL;
void(*R_BeginRegistration)(const char *map) = NULL;
void(*R_SetSky)(const char *name, float rotate, int autorotate, const vec3_t axis) = NULL;
void(*R_EndRegistration)(void) = NULL;
void(*R_RenderFrame)(refdef_t *fd) = NULL;
void(*R_LightPoint)(const vec3_t origin, vec3_t light) = NULL;
void(*R_ClearColor)(void) = NULL;
void(*R_SetAlpha)(float clpha) = NULL;
void(*R_SetAlphaScale)(float alpha) = NULL;
void(*R_SetColor)(uint32_t color) = NULL;
void(*R_SetClipRect)(const clipRect_t *clip) = NULL;
void(*R_SetScale)(float scale) = NULL;
void(*R_DrawChar)(int x, int y, int flags, int ch, qhandle_t font) = NULL;
int(*R_DrawString)(int x, int y, int flags, size_t maxChars,
	const char *string, qhandle_t font) = NULL;
void(*R_DrawPic)(int x, int y, qhandle_t pic) = NULL;
void(*R_DrawStretchPic)(int x, int y, int w, int h, qhandle_t pic) = NULL;
void(*R_DrawKeepAspectPic)(int x, int y, int w, int h, qhandle_t pic) = NULL;
void(*R_DrawStretchRaw)(int x, int y, int w, int h) = NULL;
void(*R_TileClear)(int x, int y, int w, int h, qhandle_t pic) = NULL;
void(*R_DrawFill8)(int x, int y, int w, int h, int c) = NULL;
void(*R_DrawFill32)(int x, int y, int w, int h, uint32_t color) = NULL;
void(*R_UpdateRawPic)(int pic_w, int pic_h, const uint32_t *pic) = NULL;
void(*R_DiscardRawPic)(void) = NULL;
void(*R_BeginFrame)(void) = NULL;
void(*R_EndFrame)(void) = NULL;
void(*R_ModeChanged)(int width, int height, int flags) = NULL;
void(*R_AddDecal)(decal_t *d) = NULL;
bool(*R_InterceptKey)(unsigned key, bool down) = NULL;
bool(*R_IsHDR)(void) = NULL;

bool (*R_SupportsDebugLines)(void) = NULL;
void (*R_AddDebugText_)(const vec3_t origin, const vec3_t angles, const char *text,
                        float size, uint32_t color, uint32_t time, bool depth_test) = NULL;

void(*IMG_Unload)(image_t *image) = NULL;
void(*IMG_Load)(image_t *image, byte *pic) = NULL;
void(*IMG_ReadPixels)(screenshot_t *s) = NULL;
void(*IMG_ReadPixelsHDR)(screenshot_t *s) = NULL;

int(*MOD_LoadMD2)(model_t *model, const void *rawdata, size_t length, const char* mod_name) = NULL;
#if USE_MD3
int(*MOD_LoadMD3)(model_t *model, const void *rawdata, size_t length, const char* mod_name) = NULL;
#endif
int(*MOD_LoadIQM)(model_t* model, const void* rawdata, size_t length, const char* mod_name) = NULL;
void(*MOD_Reference)(model_t *model) = NULL;

int get_auto_scale(void)
{
    int scale = 1;

    if (r_config.height < r_config.width) {
        if (r_config.height >= 2160)
            scale = 4;
        else if (r_config.height >= 1080)
            scale = 2;
    } else {
        if (r_config.width >= 3840)
            scale = 4;
        else if (r_config.width >= 1920)
            scale = 2;
    }

    if (vid.get_dpi_scale) {
        int min_scale = vid.get_dpi_scale();
        return max(scale, min_scale);
    }

    return scale;
}

float R_ClampScale(cvar_t *var)
{
    if (!var)
        return 1.0f;

    if (var->value)
        return 1.0f / Cvar_ClampValue(var, 1.0f, 10.0f);

    return 1.0f / get_auto_scale();
}

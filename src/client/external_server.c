/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2021 Frank Richter

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

/* SV Functions called from client code, either forwarding to actual server
 * code, or dealing with the "separate server process" case */

#include "shared/shared.h"
#include "server/server.h"

#undef SV_ErrorEvent
#undef SV_Init
#undef SV_Shutdown
#undef SV_Frame

void SV_ErrorEvent(netadr_t *from, int ee_errno, int ee_info);
void SV_Init(void);
void SV_Shutdown(const char *finalmsg, error_type_t type);
unsigned SV_Frame(unsigned msec);

#if (defined _WIN64)
#define ENABLE_SERVER_PROCESS
#endif

#if defined(ENABLE_SERVER_PROCESS)
static bool using_external_server;

extern cvar_t *fs_game;
extern cvar_t *sys_forcegamelib;
extern cvar_t *sys_libdir;

static bool game_library_exists(const char* game, const char* prefix, const char* cpu_str)
{
    char path[MAX_OSPATH];
    size_t len;

    len = Q_concat(path, sizeof(path), sys_libdir->string,
                   PATH_SEP_STRING, game, PATH_SEP_STRING,
                   prefix, "game", cpu_str, LIBSUFFIX, NULL);
    if (len >= sizeof(path)) {
        return false;
    }

    return os_access(path, F_OK) == 0;
}

static bool have_cpu_gamelib(const char* game, const char* cpu_str)
{
    return game_library_exists(game, "q2pro_", cpu_str) || game_library_exists(game, "", cpu_str);
}
#endif

void SV_ErrorEvent_InClient(netadr_t *from, int ee_errno, int ee_info)
{
    // FIXME: Overriding this isn't actually needed?
    SV_ErrorEvent(from, ee_errno, ee_info);
}

void SV_Init_InClient(void)
{
#if !defined(ENABLE_SERVER_PROCESS)
    SV_Init();
#else
    bool have_native_gamelib = false;
    bool have_x86_gamelib = false;
    const char *game_str = fs_game->string[0] ? fs_game->string : BASEGAME;

    // Note: This should check the same game library names as SV_InitGameProgs!
    if (sys_forcegamelib->string[0] && (os_access(sys_forcegamelib->string, F_OK) == 0))
    {
        have_native_gamelib = true;
    }
    else
    {
        have_native_gamelib = have_cpu_gamelib(game_str, CPUSTRING);
    }
    if(!have_native_gamelib)
    {
        have_x86_gamelib = have_cpu_gamelib(game_str, "x86");
    }

    /* Default logic if we have a native gamelib, or none at all.
     * (Will generate an error message in the latter case.) */
    if (have_native_gamelib || (!have_native_gamelib && !have_x86_gamelib))
    {
        SV_Init();
        return;
    }

    // We don't have a native gamelib, but an x86 one we can start a server for!
    using_external_server = true;
    // TODO: Start separate server process
    // TODO: Other sensible things to keep up appearances?
#endif
}

void SV_Shutdown_InClient(const char *finalmsg, error_type_t type)
{
#if !defined(ENABLE_SERVER_PROCESS)
    SV_Shutdown(finalmsg, type);
#else
    if(!using_external_server)
        SV_Shutdown(finalmsg, type);
    using_external_server = false;
#endif
}

unsigned SV_Frame_InClient(unsigned msec)
{
#if !defined(ENABLE_SERVER_PROCESS)
    return SV_Frame(msec);
#else
    if(!using_external_server)
        return SV_Frame(msec);

    return UINT_MAX; // force CL_Frame() result to have precedence
#endif
}

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

void SV_ErrorEvent_InClient(netadr_t *from, int ee_errno, int ee_info)
{
    SV_ErrorEvent(from, ee_errno, ee_info);
}

void SV_Init_InClient(void)
{
    SV_Init();
}

void SV_Shutdown_InClient(const char *finalmsg, error_type_t type)
{
    SV_Shutdown(finalmsg, type);
}

unsigned SV_Frame_InClient(unsigned msec)
{
    return SV_Frame(msec);
}

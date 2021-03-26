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
#include "common/zone.h"
#include "client/client.h"
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
#include <windows.h>

struct external_server_s
{
    bool active;
    HANDLE process_handle;
    // Pipe for input of external process (write to it)
    HANDLE in_pipe;
    // Pipe for output of external process (read from it)
    HANDLE out_pipe;

    char *input_buffer;
    size_t input_buffer_pos;
    size_t input_buffer_size;
} external_server;

extern cvar_t *fs_game;
extern cvar_t *sys_forcegamelib;
extern cvar_t *sys_libdir;
extern cvar_t *sys_basedir;
extern cvar_t *sys_homedir;

static char* Q_asprintf(const char *fmt, ...)
{
    va_list argptr;
    size_t  ret;

    va_start(argptr, fmt);
    ret = Q_vsnprintf(NULL, 0, fmt, argptr);

    if(ret < 0) {
        va_end(argptr);
        return NULL;
    }
    char *buf = Z_Malloc(ret + 1);
    Q_vsnprintf(buf, ret + 1, fmt, argptr);
    va_end(argptr);
    return buf;
}

struct cmd_cvar_arg_s
{
    const char *name;
    const char *value;
};

static char* assemble_command_line(const char* exe, const struct cmd_cvar_arg_s cvar_args[], int num_cvar_args)
{
    size_t need_size = strlen(exe) + 3;
    const char cvar_arg_fmt[] = " +set %s \"%s\"";
    for (int i = 0; i < num_cvar_args; i++) {
        need_size += strlen(cvar_args[i].name) + strlen(cvar_args[i].value) + sizeof(cvar_arg_fmt); // overestimation
    }
    char *buf = Z_Malloc(need_size);
    size_t buf_remaining = need_size;
    if(!buf)
        return NULL;
    char *buf_ptr = buf;
    int n;
    n = Q_snprintf(buf_ptr, buf_remaining, "\"%s\"", exe);
    buf_ptr += n;
    buf_remaining -= n;
    for (int i = 0; i < num_cvar_args; i++) {
        n = Q_snprintf(buf_ptr, buf_remaining, cvar_arg_fmt, cvar_args[i].name, cvar_args[i].value);
        buf_ptr += n;
        buf_remaining -= n;
    }
    return buf;
}

static void print_last_error(const char* call)
{
    DWORD last_error = GetLastError();
    char *message;
    DWORD msg_len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, last_error, LANG_NEUTRAL, (LPSTR)&message, 0, NULL);
    if (msg_len != 0) {
        while ((msg_len > 0) && ((message[msg_len - 1] == '\n') || (message[msg_len - 1] == '\r'))) {
            --msg_len;
            message[msg_len] = 0;
        }
        Com_EPrintf("%s failed with error: %s (%u)\n", call, message, last_error);
        LocalFree(message);
    } else {
        Com_EPrintf("%s failed with error (%u)\n", call, last_error);
    }
}

static bool start_external_server(const char* game_str)
{
    bool result = false;
    char exe_dir[MAX_PATH];
    if(!GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir))) {
        print_last_error("GetModuleFileName()");
        goto fail;
    }

    char *sep = strrchr(exe_dir, '\\');
    if(sep != NULL)
        *sep = 0;

    const char server_exe_name[] = "q2rtxded-x86.exe";
    char* server_exe_path = Q_asprintf("%s\\%s", exe_dir, server_exe_name);
    if (!server_exe_path)
        goto fail;

    const struct cmd_cvar_arg_s cvar_args[] = {
        {"sys_console", "1"},
        {"sv_external_server", "1"},
        {"basedir", sys_basedir->string},
        {"libdir", sys_libdir->string},
        {"homedir", sys_homedir->string},
        {"game", game_str}
    };

    char *cmdline = assemble_command_line(server_exe_path, cvar_args, q_countof(cvar_args));
    // TODO: Stuff like: "local singleplayer server"?
    if (!cmdline) {
        goto fail;
    }

    HANDLE input_pipe[2], output_pipe[2];
    SECURITY_ATTRIBUTES pipe_security = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    if(!CreatePipe(&input_pipe[0], &input_pipe[1], &pipe_security, 0)) {
        print_last_error("CreatePipe()");
        goto fail;
    }
    if(!CreatePipe(&output_pipe[0], &output_pipe[1], &pipe_security, 0)) {
        print_last_error("CreatePipe()");
        goto fail1;
    }

    STARTUPINFOEXA startup_info;
    memset(&startup_info, 0, sizeof(startup_info));
    startup_info.StartupInfo.cb = sizeof(startup_info);
    startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup_info.StartupInfo.hStdInput = input_pipe[0];
    startup_info.StartupInfo.hStdOutput = output_pipe[1];
    startup_info.StartupInfo.hStdError = output_pipe[1];

    HANDLE inherit_handles[] = {input_pipe[0], output_pipe[1]};
    SIZE_T proc_thread_attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &proc_thread_attr_size);
    startup_info.lpAttributeList = Z_Malloc(proc_thread_attr_size);
    if(!InitializeProcThreadAttributeList(startup_info.lpAttributeList, 1, 0, &proc_thread_attr_size)) {
        print_last_error("InitializeProcThreadAttributeList()");
        goto fail2;
    }
    if(!UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherit_handles, sizeof(inherit_handles), NULL, NULL)) {
        print_last_error("UpdateProcThreadAttribute()");
        goto fail3;
    }

    PROCESS_INFORMATION process_info;
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, (LPSTARTUPINFOA)&startup_info, &process_info)) {
        print_last_error("CreateProcess()");
        goto fail3;
    }

    external_server.process_handle = process_info.hProcess;
    CloseHandle(process_info.hThread);
    external_server.in_pipe = input_pipe[1];
    external_server.out_pipe = output_pipe[0];
    external_server.active = true;

    external_server.input_buffer_size = 1024;
    external_server.input_buffer = Z_Malloc(external_server.input_buffer_size);
    external_server.input_buffer_pos = 0;

    result = true;

fail3:
    DeleteProcThreadAttributeList(startup_info.lpAttributeList);
fail2:
    Z_Free(cmdline);
    Z_Free(startup_info.lpAttributeList);

    if(!result) {
        CloseHandle(output_pipe[0]);
    }
    CloseHandle(output_pipe[1]);
fail1:
    CloseHandle(input_pipe[0]);
    if(!result) {
        CloseHandle(input_pipe[1]);
    }
fail:
    return result;
}

static void send_server_command(const char* cmd)
{
    if (!external_server.active)
        return;

    char *command = Q_asprintf("%s\n", cmd);
    DWORD bytes_written;
    if(!WriteFile(external_server.in_pipe, command, strlen(command), &bytes_written, NULL)
        || (bytes_written != strlen(command))) {
        print_last_error("WriteFile()");
    }
    Z_Free(command);
}

static void end_external_server(void)
{
    if (!external_server.active)
        return;

    send_server_command("quit");

#if !defined(_DEBUG)
    DWORD timeout = 5000;
    if (WaitForSingleObject(external_server.process_handle, timeout) != WAIT_OBJECT_0) {
        TerminateProcess(external_server.process_handle, ERROR_TIMEOUT);
    }
#else
    DWORD timeout = 30000;
    if (WaitForSingleObject(external_server.process_handle, timeout) != WAIT_OBJECT_0) {
        Com_WPrintf("external server did not quit after %u ms\n", timeout);
    }
#endif
    DWORD exit_code;
    if(!GetExitCodeProcess(external_server.process_handle, &exit_code)) {
        print_last_error("GetExitCodeProcess()");
    } else if (exit_code != 0) {
        Com_LPrintf(PRINT_NOTICE, "external server exited with code %u\n", exit_code);
    }
    CloseHandle(external_server.process_handle);
    CloseHandle(external_server.in_pipe);
    CloseHandle(external_server.out_pipe);

    Z_Free(external_server.input_buffer);

    memset(&external_server, 0, sizeof(external_server));
}

static char* strnchr(char* str, size_t n, int c)
{
    while(n-- > 0) {
        if (*str == c)
            return str;
        ++str;
    }
    return NULL;
}

static void forward_external_server_output(void)
{
    DWORD bytes_avail = 0;
    if(!PeekNamedPipe(external_server.out_pipe, NULL, 0, NULL, &bytes_avail, 0) || (bytes_avail == 0))
        return;

    size_t buf_remaining = external_server.input_buffer_size - external_server.input_buffer_pos;
    if (bytes_avail > buf_remaining)
    {
        size_t new_size = external_server.input_buffer_pos + bytes_avail;
        external_server.input_buffer = Z_Realloc(external_server.input_buffer, new_size);
        external_server.input_buffer_size = new_size;
        buf_remaining = external_server.input_buffer_size - external_server.input_buffer_pos;
    }
    DWORD bytes_read = 0;
    if(!ReadFile(external_server.out_pipe, external_server.input_buffer + external_server.input_buffer_pos, bytes_avail, &bytes_read, NULL))
        return;
    external_server.input_buffer_pos += bytes_read;

    // Line-wise output to console
    char *buf_pos = external_server.input_buffer;
    size_t scan_size = external_server.input_buffer_pos;
    char *linesep = strnchr(buf_pos, scan_size, '\n');
    while(linesep != NULL) {
        // Need to null-terminate...
        *linesep = 0;
        Con_Printf("%s\n", buf_pos);
        size_t line_len = linesep - buf_pos + 1;
        buf_pos += line_len;
        scan_size -= line_len;
        linesep = strnchr(buf_pos, scan_size, '\n');
    }
    // Remove printed lines from buffer
    if(buf_pos > external_server.input_buffer) {
        size_t remainder = (external_server.input_buffer + external_server.input_buffer_pos) - buf_pos;
        memmove(external_server.input_buffer, buf_pos, remainder);
        external_server.input_buffer_pos = remainder;
    }
}

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
    memset(&external_server, 0, sizeof(external_server));

    bool have_native_gamelib = false;
    bool have_x86_gamelib = false;
    const char *game_str = fs_game->string[0] ? fs_game->string : BASEGAME;

    // Note: This should check the same game library names as SV_InitGameProgs!
    if (sys_forcegamelib->string[0] && (os_access(sys_forcegamelib->string, F_OK) == 0)) {
        have_native_gamelib = true;
    } else {
        have_native_gamelib = have_cpu_gamelib(game_str, CPUSTRING);
    }
    if(!have_native_gamelib) {
        have_x86_gamelib = have_cpu_gamelib(game_str, "x86");
    }

    if (!have_native_gamelib && have_x86_gamelib) {
        // Try to launch external server for x86 gamelib
        if (start_external_server(game_str))
            return;
        // TODO: Start separate server process
        // TODO: Other sensible things to keep up appearances?
    }

    /* Default logic if we have a native gamelib, or none at all.
     * (Will generate an error message in the latter case.) */
    SV_Init();
#endif
}

void SV_Shutdown_InClient(const char *finalmsg, error_type_t type)
{
#if !defined(ENABLE_SERVER_PROCESS)
    SV_Shutdown(finalmsg, type);
#else
    if(!external_server.active) {
        SV_Shutdown(finalmsg, type);
        return;
    }

    end_external_server();
#endif
}

unsigned SV_Frame_InClient(unsigned msec)
{
#if !defined(ENABLE_SERVER_PROCESS)
    return SV_Frame(msec);
#else
    if(!external_server.active)
        return SV_Frame(msec);

    forward_external_server_output();

    return msec; // force CL_Frame() result to have precedence
#endif
}

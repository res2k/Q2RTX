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
#include "common/cvar.h"
#include "common/compat_server_proto.h"
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

struct compat_server_process_s
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
} compat_server_process;

#if defined(_DEBUG)
extern cvar_t *developer;
#endif
extern cvar_t *fs_game;
extern cvar_t *sys_forcegamelib;
extern cvar_t *sys_libdir;
extern cvar_t *sys_basedir;
extern cvar_t *sys_homedir;

// cvar argument for compatibility server process
struct cmd_cvar_arg_s
{
    const char *name;
    const char *value;
};

// Support for dynamic struct cmd_cvar_arg_s array
struct cmd_cvar_arg_array_s
{
    struct cmd_cvar_arg_s* elements;
    size_t count;
    size_t reserved;
};

static void cmd_cvar_arg_array_init(struct cmd_cvar_arg_array_s *array)
{
    array->reserved = 8;
    array->count = 0;
    array->elements = Z_Malloc(array->reserved * sizeof(struct cmd_cvar_arg_s));
}

static void cmd_cvar_arg_array_free(struct cmd_cvar_arg_array_s *array)
{
    Z_Free(array->elements);
}

static void cmd_cvar_arg_array_append(struct cmd_cvar_arg_array_s *array, const char *name, const char *value)
{
    if(array->count + 1 > array->reserved) {
        array->reserved += 8;
        array->elements = Z_Realloc(array->elements, array->reserved * sizeof(struct cmd_cvar_arg_s));
    }
    array->elements[array->count].name = name;
    array->elements[array->count].value = value;
    array->count++;
}

// Assemble a command line for the compatibility server process
static wchar_t* assemble_command_line(const wchar_t* exe, const struct cmd_cvar_arg_s* cvar_args, size_t num_cvar_args)
{
    // Format all args into a single string
    size_t need_args_size = 0;
    const char cvar_arg_fmt[] = " +set %s \"%s\"";
    for (int i = 0; i < num_cvar_args; i++) {
        need_args_size += strlen(cvar_args[i].name) + strlen(cvar_args[i].value) + sizeof(cvar_arg_fmt); // overestimation
    }
    need_args_size++;
    char *args_buf = _alloca(need_args_size);
    char *args_ptr = args_buf;
    size_t args_remaining = need_args_size;
    for (int i = 0; i < num_cvar_args; i++) {
        int n = Q_snprintf(args_ptr, args_remaining, cvar_arg_fmt, cvar_args[i].name, cvar_args[i].value);
        args_ptr += n;
        args_remaining -= n;
    }

    int args_size_wide = MultiByteToWideChar(CP_ACP, 0, args_buf, -1, NULL, 0);
    size_t buf_wide_size = wcslen(exe) + 2 + args_size_wide;
    wchar_t *buf_wide = Z_Malloc(buf_wide_size * sizeof(wchar_t));
    _snwprintf(buf_wide, buf_wide_size, L"\"%s\"", exe);
    MultiByteToWideChar(CP_UTF8, 0, args_buf, -1, buf_wide + wcslen(buf_wide), args_size_wide);

    return buf_wide;
}

// Print the Windows last error to the console
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

// Return application exe path. Allocates memory from zone
static wchar_t* get_app_exe_path(void)
{
    wchar_t *exe_path = NULL;
    size_t buf_size = MAX_PATH;
    while(true) {
        exe_path = Z_Realloc(exe_path, (buf_size + 1) * sizeof(wchar_t));
        DWORD result_len = GetModuleFileNameW(NULL, exe_path, buf_size);
        if(result_len == 0) {
            // an error occures
            print_last_error("GetModuleFileName()");
            return NULL;
        } else if(result_len < buf_size) {
            // Path fits into buffer, return
            return exe_path;
        } else {
            // Path didn't fit, try again with bigger buffer
            buf_size *= 2;
        }
    }
}

// Start a compatibility server process for the given game
static bool start_compat_server_process(const char* game_str)
{
    bool result = false;
    wchar_t *server_exe_path = get_app_exe_path();
    if(!server_exe_path)
        goto fail;

    wchar_t *sep = wcsrchr(server_exe_path, '\\');
    if(sep != NULL)
        *(sep + 1) = 0;
    else
        *server_exe_path = 0;

    const wchar_t server_exe_name[] = L"q2rtxcsp-x86.exe";
    server_exe_path = Z_Realloc(server_exe_path, (wcslen(server_exe_path) + wcslen(server_exe_name) + 1) * sizeof(wchar_t));
    wcscat(server_exe_path, server_exe_name);

    struct cmd_cvar_arg_array_s cvar_args;
    cmd_cvar_arg_array_init(&cvar_args);
#if defined(_DEBUG)
    if(developer->integer)
        cmd_cvar_arg_array_append(&cvar_args, "developer", developer->string);
#endif
    cvar_t *sys_disablecrashdump = Cvar_Get("sys_disablecrashdump", "0", CVAR_NOSET);
    if(sys_disablecrashdump->integer)
        cmd_cvar_arg_array_append(&cvar_args, "sys_disablecrashdump", sys_disablecrashdump->string);
    cmd_cvar_arg_array_append(&cvar_args, "sys_console", "1");
    cmd_cvar_arg_array_append(&cvar_args, "basedir", sys_basedir->string);
    cmd_cvar_arg_array_append(&cvar_args, "libdir", sys_libdir->string);
    cmd_cvar_arg_array_append(&cvar_args, "homedir", sys_homedir->string);
    cmd_cvar_arg_array_append(&cvar_args, "game", game_str);

    wchar_t *cmdline = assemble_command_line(server_exe_path, cvar_args.elements, cvar_args.count);
    cmd_cvar_arg_array_free(&cvar_args);
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

    STARTUPINFOEXW startup_info;
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
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, (LPSTARTUPINFOW)&startup_info, &process_info)) {
        print_last_error("CreateProcess()");
        goto fail3;
    }

    compat_server_process.process_handle = process_info.hProcess;
    CloseHandle(process_info.hThread);
    compat_server_process.in_pipe = input_pipe[1];
    compat_server_process.out_pipe = output_pipe[0];
    compat_server_process.active = true;

    compat_server_process.input_buffer_size = 1024;
    compat_server_process.input_buffer = Z_Malloc(compat_server_process.input_buffer_size);
    compat_server_process.input_buffer_pos = 0;

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
    Z_Free(server_exe_path);
    return result;
}

// Send a command string to the compatibility server
static void send_server_command(const char* cmd)
{
    if (!compat_server_process.active)
        return;

    char *command = _alloca(strlen(cmd) + 2);
    strcpy(command, cmd);
    strcat(command, "\n");
    DWORD bytes_written;
    if(!WriteFile(compat_server_process.in_pipe, command, strlen(command), &bytes_written, NULL)
        || (bytes_written != strlen(command))) {
        print_last_error("WriteFile()");
    }
}

// Instruct the compatibility server process to end itself
static void end_compat_server_process(void)
{
    if (!compat_server_process.active)
        return;

    send_server_command("quit");

#if !defined(_DEBUG)
    // Terminate process if compat server doesn't react
    DWORD timeout = 5000;
    if (WaitForSingleObject(compat_server_process.process_handle, timeout) != WAIT_OBJECT_0) {
        TerminateProcess(compat_server_process.process_handle, ERROR_TIMEOUT);
    }
#else
    // Debug mode: Wait longer and don't terminate. Helpful if a debugger was attached
    DWORD timeout = 30000;
    if (WaitForSingleObject(compat_server_process.process_handle, timeout) != WAIT_OBJECT_0) {
        Com_WPrintf("external server did not quit after %u ms\n", timeout);
    }
#endif
    DWORD exit_code;
    if(!GetExitCodeProcess(compat_server_process.process_handle, &exit_code)) {
        print_last_error("GetExitCodeProcess()");
    } else if (exit_code != 0) {
        Com_LPrintf(PRINT_NOTICE, "external server exited with code %u\n", exit_code);
    }
    CloseHandle(compat_server_process.process_handle);
    CloseHandle(compat_server_process.in_pipe);
    CloseHandle(compat_server_process.out_pipe);

    Z_Free(compat_server_process.input_buffer);

    memset(&compat_server_process, 0, sizeof(compat_server_process));
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

static void handle_compat_server_msg(struct compat_server_msg_s* msg)
{
    switch(msg->op) {
    case cso_con_output:
        {
            print_type_t print_type = *msg->payload - '0';
            Com_LPrintf(print_type, "%s", msg->payload + 1);
            break;
        }
    }
}

// Grab output from the compatibility server process, print to console
static void forward_compat_server_process_output(void)
{
    DWORD bytes_avail = 0;
    while(PeekNamedPipe(compat_server_process.out_pipe, NULL, 0, NULL, &bytes_avail, 0) && (bytes_avail != 0)) {
        char buf[256];
        const DWORD max_read = sizeof(buf);
        DWORD read_size = bytes_avail > max_read ? max_read : bytes_avail;
        DWORD bytes_read = 0;
        if(!ReadFile(compat_server_process.out_pipe, buf, read_size, &bytes_read, NULL)) {
            print_last_error("ReadFile()");
            goto protocol_error;
        }

        char *buf_end = buf + bytes_read;
        char *buf_ptr = buf;

        while(buf_ptr < buf_end) {
            struct compat_server_msg_s msg;
            size_t data_consumed, data_required;
            if (!CompatServer_BeginParseMsg(&msg, buf_ptr, buf_end - buf_ptr, &data_consumed, &data_required)) {
                goto protocol_error;
            }
            buf_ptr += data_consumed;
            while(data_required > 0) {
                if(buf_ptr == buf_end)
                {
                    DWORD read_size = data_required > max_read ? max_read : data_required;
                    DWORD bytes_read = 0;
                    if(!ReadFile(compat_server_process.out_pipe, buf, read_size, &bytes_read, NULL)) {
                        print_last_error("ReadFile()");
                        CompatServer_FreeMsg(&msg);
                        goto protocol_error;
                    }
                    buf_end = buf + bytes_read;
                    buf_ptr = buf;
                }
                size_t data_size = buf_end - buf_ptr;
                if (data_size > data_required)
                    data_size = data_required;
                if (!CompatServer_AddMsgData(&msg, buf_ptr, data_size, &data_required)) {
                    CompatServer_FreeMsg(&msg);
                    goto protocol_error;
                }
                buf_ptr += data_size;
            }

            handle_compat_server_msg(&msg);

            CompatServer_FreeMsg(&msg);
        }
    }

    return;

protocol_error:
    // "Out of sync"
    end_compat_server_process();
}

// Check whether a specific game library exists
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

// Check whether a supported game library exists for the given CPU
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

static bool need_compat_server_process;
static const char *game_string;

void SV_Init_InClient(void)
{
#if !defined(ENABLE_SERVER_PROCESS)
    SV_Init();
#else
    memset(&compat_server_process, 0, sizeof(compat_server_process));

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
        need_compat_server_process = true;
    }

    game_string = game_str;

    if (need_compat_server_process && start_compat_server_process(game_str))
        return;

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
    if(!compat_server_process.active) {
        SV_Shutdown(finalmsg, type);
        return;
    }

    if(type == ERR_DISCONNECT) {
        /* Try to guess reason for disconnect from message (hacky),
         * adjust behaviour */
        if (strstr(finalmsg, "quit") != NULL) {
            // Quit: Exit external process
            end_compat_server_process();
            return;
        } else if (strstr(finalmsg, "Server disconnected") != NULL) {
            /* Disconnected by server: don't do anything.
             * Especially not "killserver", since the server may still be running
             * and we'll auto-connect back */
            return;
        }
    }

    if (type == ERR_FATAL) {
        end_compat_server_process();
    } else {
        // Non-fatal default: issue "killserver" command
        send_server_command("killserver");
    }

#endif
}

unsigned SV_Frame_InClient(unsigned msec)
{
#if !defined(ENABLE_SERVER_PROCESS)
    return SV_Frame(msec);
#else
    if(!compat_server_process.active)
        return SV_Frame(msec);

    forward_compat_server_process_output();

    return msec; // force CL_Frame() result to have precedence
#endif
}

bool CL_ForwardToCompatServer(void)
{
    // Restart external server process, if necessary
    if(need_compat_server_process && !compat_server_process.active) {
        if(!start_compat_server_process(game_string))
            return false;
    }

    send_server_command(Cmd_RawArgsFrom(0));
    return true;
}

bool CL_ServerIsCompat(void)
{
    return need_compat_server_process;
}

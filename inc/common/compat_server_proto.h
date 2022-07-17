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

/* "Protocol" for compatibility server output (via stdout) */

#ifndef COMPAT_SERVER_PROTO_H_
#define COMPAT_SERVER_PROTO_H_

enum compat_server_op_t {
    cso_con_output = 'C',
    cso_cvar_change = 'V',
    cso_command_result = 'M',
    cso_loading_plaque = 'P'
};

// Output functions
void CompatServer_ConsoleOutput(print_type_t print_type, const char *msg);
void CompatServer_CvarChange(struct cvar_s *cvar);
void CompatServer_CommandResult(const char *cmd, bool result);
void CompatServer_LoadingPlaque(bool show);

// Input functions

struct compat_server_msg_s {
    enum compat_server_op_t op;
    char *payload;

    // Parse state handling ("private")
    int _parse_state;
    char *_raw_msg;
    char *_raw_msg_end;
    char *_raw_msg_ptr;
    union {
        struct {
            size_t start_pos;
            size_t end_pos;
        } payload_len;
        struct {
            size_t start_pos;
        } payload;
    } _parse_data;

    size_t _parsed_payload_length;
};

/* Begin parsing.
   Pass in the available data and it's size.
   The function will consume all or a part of it (indicated by the data_consumed value),
   and may require additional data (indicated by min_required_data).
   The input buffer should be advanced by 'data_consumed' and any subsequent parsing (new message
   or continuation) should drain the remaining buffer data first. */
bool CompatServer_BeginParseMsg(struct compat_server_msg_s *msg, const char *data, size_t data_size, size_t* data_consumed, size_t *min_required_data);
/* Continue parsing.
   The function will consume all the data provided (you should at most pass in the earlier requested amount of data),
   but may require additional data (indicated by required_data). */
bool CompatServer_AddMsgData(struct compat_server_msg_s *msg, const char *data, size_t data_size, size_t *required_data);
// Free data associated with parsing
void CompatServer_FreeMsg(struct compat_server_msg_s *msg);

#endif // COMPAT_SERVER_PROTO_H_

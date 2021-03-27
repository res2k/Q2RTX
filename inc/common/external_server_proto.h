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

/* "Protocol" for external server output (via stdout) */

#ifndef EXTERNAL_SERVER_PROTO_H_
#define EXTERNAL_SERVER_PROTO_H_

enum external_server_op_t {
    eso_con_output = 'C'
};

// Output functions
void ExternalServer_ConsoleOutput(const char *msg);

// Input functions

struct external_server_msg_s {
    enum external_server_op_t op;
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
bool ExternalServer_BeginParseMsg(struct external_server_msg_s *msg, const char *data, size_t data_size, size_t* data_consumed, size_t *min_required_data);
/* Continue parsing.
   The function will consume all the data provided (you should at most pass in the earlier requested amount of data),
   but may require additional data (indicated by required_data). */
bool ExternalServer_AddMsgData(struct external_server_msg_s *msg, const char *data, size_t data_size, size_t *required_data);
void ExternalServer_FreeMsg(struct external_server_msg_s *msg);

#endif // EXTERNAL_SERVER_PROTO_H_

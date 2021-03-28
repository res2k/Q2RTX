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

#include "shared/shared.h"
#include "common/compat_server_proto.h"
#include "common/zone.h"
#include "system/system.h"

#include <assert.h>

/*
    Message format:
        <opchar><size (decimal)><space><raw data>
        |---------- Header -----------|| Payload ... |
 */

void CompatServer_ConsoleOutput(print_type_t print_type, const char *msg)
{
    Sys_Printf("%c%d %c%s", cso_con_output, 1 + strlen(msg), '0' + print_type, msg);
}

void CompatServer_CvarChange(struct cvar_s *cvar)
{
    Sys_Printf("%c%d %s %s", cso_cvar_change, strlen(cvar->name) + 1 + strlen(cvar->string), cvar->name, cvar->string);
}

static void parse_append_data(struct compat_server_msg_s *msg, const char *data, size_t data_size)
{
    size_t old_msg_ptr = msg->_raw_msg_ptr - msg->_raw_msg;
    if(msg->_raw_msg == NULL) {
        msg->_raw_msg = Z_Malloc(data_size + 1);
        memcpy(msg->_raw_msg, data, data_size);
        msg->_raw_msg_end = msg->_raw_msg + data_size;
    } else {
        size_t raw_msg_size = msg->_raw_msg_end - msg->_raw_msg;
        size_t new_size = raw_msg_size + data_size;
        msg->_raw_msg = Z_Realloc(msg->_raw_msg, new_size + 1); // +1 for null termination
        memcpy(msg->_raw_msg + raw_msg_size, data, data_size);
        msg->_raw_msg_end = msg->_raw_msg + new_size;
    }
    msg->_raw_msg_ptr = msg->_raw_msg + old_msg_ptr;
}

enum parse_state {
    ps_op = 0,
    ps_payload_len,
    ps_payload,

    ps_NUM
};

enum parse_result {
    pr_fail = 0,
    pr_success,
    pr_continue,
    pr_need_more_data
};

static enum parse_result advance_parse_state(struct compat_server_msg_s *msg, enum parse_state new_state)
{
    msg->_parse_state = new_state;
    memset(&msg->_parse_data, 0, sizeof(msg->_parse_data));
    return pr_continue;
}

static enum parse_result parse_op(struct compat_server_msg_s *msg, size_t* consumed, size_t* required)
{
    *consumed = 0;
    *required = 0;
    size_t data_avail = msg->_raw_msg_end - msg->_raw_msg_ptr;
    if (data_avail == 0) {
        *required = 1;
        return pr_need_more_data;
    }

    msg->op = *msg->_raw_msg_ptr;
    msg->_raw_msg_ptr++;
    *consumed = 1;
    return advance_parse_state(msg, ps_payload_len);
}

static enum parse_result parse_payload_len(struct compat_server_msg_s *msg, size_t* consumed, size_t* required)
{
    *consumed = 0;
    *required = 0;
    if(msg->_parse_data.payload_len.start_pos == 0)
        msg->_parse_data.payload_len.start_pos = msg->_raw_msg_ptr - msg->_raw_msg;
    while(msg->_raw_msg_ptr < msg->_raw_msg_end)
    {
        if (*msg->_raw_msg_ptr == ' ') {
            msg->_parse_data.payload_len.end_pos = msg->_raw_msg_ptr - msg->_raw_msg;
            break;
        }
        msg->_raw_msg_ptr++;
        (*consumed)++;
    }
    if (msg->_parse_data.payload_len.end_pos <= msg->_parse_data.payload_len.start_pos) {
        // Did not reach end of payload length yet
        *required = 1;
        return pr_need_more_data;
    }
    // Advance past space
    msg->_raw_msg_ptr++;
    (*consumed)++;

    // Convert data
    *(msg->_raw_msg + msg->_parse_data.payload_len.end_pos) = 0;
    unsigned int len;
    if(sscanf(msg->_raw_msg + msg->_parse_data.payload_len.start_pos, "%u", &len) != 1) {
        return pr_fail;
    }
    msg->_parsed_payload_length = len;
    return advance_parse_state(msg, ps_payload);
}

static enum parse_result parse_payload(struct compat_server_msg_s *msg, size_t* consumed, size_t* required)
{
    if(msg->_parse_data.payload.start_pos == 0)
        msg->_parse_data.payload.start_pos = msg->_raw_msg_ptr - msg->_raw_msg;
    size_t total_payload = msg->_raw_msg_end - (msg->_raw_msg + msg->_parse_data.payload.start_pos);
    if(total_payload < msg->_parsed_payload_length) {
        size_t raw_msg_remaining = msg->_raw_msg_end - msg->_raw_msg_ptr;
        *consumed = raw_msg_remaining;
        *required = msg->_parsed_payload_length - total_payload;
        msg->_raw_msg_ptr += raw_msg_remaining;
        return pr_need_more_data;
    } else {
        *consumed = msg->_raw_msg + msg->_parse_data.payload.start_pos + msg->_parsed_payload_length - msg->_raw_msg_ptr;
        *required = 0;
        msg->_raw_msg_ptr += *consumed;
        *msg->_raw_msg_ptr = 0; // null terminate payload
        msg->payload = msg->_raw_msg + msg->_parse_data.payload.start_pos;
        return pr_success;
    }
}

typedef enum parse_result (*parse_func_t)(struct compat_server_msg_s *, size_t*, size_t*);

static parse_func_t parse_functions[ps_NUM] = {parse_op, parse_payload_len, parse_payload};

static bool parse_loop(struct compat_server_msg_s *msg, const char *data, size_t data_size, size_t* data_consumed, size_t *min_required_data)
{
    enum parse_result pr = pr_continue;

    size_t total_consumed = 0;
    while (pr == pr_continue) {
        parse_func_t parse_func = parse_functions[msg->_parse_state];
        size_t step_consumed, step_required;
        pr = parse_func(msg, &step_consumed, &step_required);
        total_consumed += step_consumed;
        *min_required_data = step_required;
    }
    *data_consumed = total_consumed;

    return pr != pr_fail;
}

bool CompatServer_BeginParseMsg(struct compat_server_msg_s *msg, const char *data, size_t data_size, size_t* data_consumed, size_t *min_required_data)
{
    memset(msg, 0, sizeof(struct compat_server_msg_s));
    parse_append_data(msg, data, data_size);
    return parse_loop(msg, data, data_size, data_consumed, min_required_data) != pr_fail;
}

bool CompatServer_AddMsgData(struct compat_server_msg_s *msg, const char *data, size_t data_size, size_t *required_data)
{
    size_t data_consumed;
    parse_append_data(msg, data, data_size);
    enum parse_result pr = parse_loop(msg, data, data_size, &data_consumed, required_data);
    assert(data_consumed == data_size);
    return pr != pr_fail;
}

void CompatServer_FreeMsg(struct compat_server_msg_s *msg)
{
    Z_Free(msg->_raw_msg);
}


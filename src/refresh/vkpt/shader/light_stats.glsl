/*
Copyright (C) 2018 Tobias Zirr
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

#ifndef LIGHT_STATS_GLSL_
#define LIGHT_STATS_GLSL_

#include "hash.glsl"

uint get_light_stats_addr(uint cluster, uint light, uint side)
{
    uint addr = cluster;
    addr = addr * global_ubo.num_static_lights + light;
    addr = addr * 6 + side;
    addr = addr * 2;
    return addr;
}

uint get_primary_direction(vec3 dir)
{
    vec3 adir = abs(dir);
    if(adir.x > adir.y && adir.x > adir.z)
        return (dir.x < 0) ? 1 : 0;
    if(adir.y > adir.z)
        return (dir.y < 0) ? 3 : 2;
    return (dir.z < 0) ? 5 : 4;
}

ivec4 light_stats_hash_cell(vec3 world_pos, vec3 jitter, vec3 cam_pos)
{
    return spatialhash_cell(world_pos, jitter, cam_pos, 0.125, 5);
}

uvec2 light_stats_hash(vec3 world_pos, vec3 jitter, vec3 cam_pos, uint light, uint side)
{
    ivec4 cell = light_stats_hash_cell(world_pos, jitter, cam_pos);
    // Pack cell down to 16 bpc
    uvec2 cell_packed = uvec2(uint(cell.x) & 0xffff | uint(cell.y) << 16, uint(cell.z) & 0xffff | uint(cell.w) << 16);
    uint light_idx = light * 6 + side;
    uint hash_1 = hash_pcg(light_idx + hash_pcg(cell_packed.y + hash_pcg(cell_packed.x)));
    uint hash_2 = hash_xxhash32(uvec3(cell_packed.x, cell_packed.y, light_idx));
    return uvec2(hash_1, hash_2);
}

int light_stats_hash_find_for_update(uint buffer_idx, uvec2 hash)
{
    int index = int(hash.x & LIGHT_STATS_HASH_MASK);
    uint checksum = hash.y;
    for(int i = 0; i < 32; i++)
    {
        uint checksum_prev = atomicCompSwap(light_stats_hash_buffers[buffer_idx].stats[index].checksum, 0, checksum);
        if (checksum_prev == 0 || checksum_prev == checksum)
            return index;
        index++;
    }
    return -1;
}

int light_stats_hash_find(uint buffer_idx, uvec2 hash)
{
    int index = int(hash.x & LIGHT_STATS_HASH_MASK);
    uint checksum = hash.y;
    for(int i = 0; i < 32; i++)
    {
        uint read_checksum = light_stats_hash_buffers[buffer_idx].stats[index].checksum;
        if (read_checksum == checksum)
            return index;
        else if (read_checksum == 0)
            break;
        index++;
    }
    return -1;
}

vec3 light_stats_hash_jitter(vec3 normal, vec2 rng)
{
    mat3x3 ts = construct_ONB_frisvad(normal);

    rng = rng * vec2(2) - vec2(1);
    vec3 jitter = ts[0] * rng.x + ts[2] * rng.y;
    jitter *= clamp(global_ubo.pt_light_stats_hash_jitter, 0, 1);
    return jitter;
}

void light_stats_hash_accumulate(vec3 world_pos, vec3 jitter, uint light, vec3 normal, bool shadowed)
{
    if(global_ubo.pt_light_stats == 0) return;

    int buffer_idx = global_ubo.current_frame_idx % NUM_LIGHT_STATS_BUFFERS;
    vec3 cam_pos = light_stats_hash_buffers[buffer_idx].header.camera_pos;
    int index = light_stats_hash_find_for_update(buffer_idx, light_stats_hash(world_pos, jitter, cam_pos, light, get_primary_direction(normal)));
    if (index == -1) return;

    if(shadowed)
        atomicAdd(light_stats_hash_buffers[buffer_idx].stats[index].misses, 1);
    else
        atomicAdd(light_stats_hash_buffers[buffer_idx].stats[index].hits, 1);
}

void light_stats_hash_get(vec3 world_pos, vec3 jitter, uint light, vec3 normal, bool is_gradient, out uint num_hits, out uint num_misses)
{
    num_hits = 0;
    num_misses = 0;
    if(global_ubo.pt_light_stats == 0)
        return;

    uint buffer_idx = global_ubo.current_frame_idx;
    // Regular pixels get shadowing stats from the previous frame;
    // Gradient pixels get the stats from two frames ago because they need to match
    // the light sampling from the previous frame.
    buffer_idx += is_gradient ? (NUM_LIGHT_STATS_BUFFERS - 2) : (NUM_LIGHT_STATS_BUFFERS - 1);
    buffer_idx = buffer_idx % NUM_LIGHT_STATS_BUFFERS;

    vec3 cam_pos = light_stats_hash_buffers[buffer_idx].header.camera_pos;
    int index = light_stats_hash_find(buffer_idx, light_stats_hash(world_pos, jitter, cam_pos, light, get_primary_direction(normal)));
    if (index == -1) return;

    num_hits = light_stats_hash_buffers[buffer_idx].stats[index].hits;
    num_misses = light_stats_hash_buffers[buffer_idx].stats[index].misses;
}


#endif // LIGHT_STATS_GLSL_

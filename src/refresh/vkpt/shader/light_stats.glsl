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

void light_stats_accumulate(uint cluster, uint light, vec3 normal, bool shadowed)
{
    if(global_ubo.pt_light_stats == 0) return;

    uint addr = get_light_stats_addr(cluster, light, get_primary_direction(normal));

    // Offset 0 is unshadowed rays,
    // Offset 1 is shadowed rays
    if(shadowed) addr += 1;

    // Increment the ray counter
    atomicAdd(light_stats_buffers[global_ubo.current_frame_idx % NUM_LIGHT_STATS_BUFFERS].stats[addr], 1);
}

void light_stats_get(uint cluster, uint light, vec3 normal, bool is_gradient, out uint num_hits, out uint num_misses)
{
    if(global_ubo.pt_light_stats == 0) {
        num_hits = 0;
        num_misses = 0;
        return;
    }

    uint buffer_idx = global_ubo.current_frame_idx;
    // Regular pixels get shadowing stats from the previous frame;
    // Gradient pixels get the stats from two frames ago because they need to match
    // the light sampling from the previous frame.
    buffer_idx += is_gradient ? (NUM_LIGHT_STATS_BUFFERS - 2) : (NUM_LIGHT_STATS_BUFFERS - 1);
    buffer_idx = buffer_idx % NUM_LIGHT_STATS_BUFFERS;

    uint addr = get_light_stats_addr(cluster, light, get_primary_direction(normal));

    num_hits = light_stats_buffers[buffer_idx].stats[addr];
    num_misses = light_stats_buffers[buffer_idx].stats[addr + 1];
}

#endif // LIGHT_STATS_GLSL_

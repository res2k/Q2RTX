/*
Copyright (C) 2025 Frank Richter

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

/*
 * Hash functions from:
 * Mark Jarzynski and Marc Olano, Hash Functions for GPU Rendering, 
 * Journal of Computer Graphics Techniques (JCGT), vol. 9, no. 3, 21-38, 2020
 * Available online http://jcgt.org/published/0009/03/02/
 */
#ifndef HASH_GLSL_
#define HASH_GLSL_

// https://www.pcg-random.org/
uint hash_pcg(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// xxhash (https://github.com/Cyan4973/xxHash)
//   From https://www.shadertoy.com/view/Xt3cDn
uint hash_xxhash32(uint p)
{
    const uint PRIME32_2 = 2246822519U, PRIME32_3 = 3266489917U;
    const uint PRIME32_4 = 668265263U, PRIME32_5 = 374761393U;
    uint h32 = p + PRIME32_5;
    h32 = PRIME32_4*((h32 << 17) | (h32 >> (32 - 17)));
    h32 = PRIME32_2*(h32^(h32 >> 15));
    h32 = PRIME32_3*(h32^(h32 >> 13));
    return h32^(h32 >> 16);
}

uint hash_xxhash32(uvec2 p)
{
    const uint PRIME32_2 = 2246822519U, PRIME32_3 = 3266489917U;
    const uint PRIME32_4 = 668265263U, PRIME32_5 = 374761393U;
    uint h32 = p.y + PRIME32_5 + p.x*PRIME32_3;
    h32 = PRIME32_4*((h32 << 17) | (h32 >> (32 - 17)));
    h32 = PRIME32_2*(h32^(h32 >> 15));
    h32 = PRIME32_3*(h32^(h32 >> 13));
    return h32^(h32 >> 16);
}

uint hash_xxhash32(uvec3 p)
{
    const uint PRIME32_2 = 2246822519U, PRIME32_3 = 3266489917U;
    const uint PRIME32_4 = 668265263U, PRIME32_5 = 374761393U;
    uint h32 =  p.z + PRIME32_5 + p.x*PRIME32_3;
    h32 = PRIME32_4*((h32 << 17) | (h32 >> (32 - 17)));
    h32 += p.y * PRIME32_3;
    h32 = PRIME32_4*((h32 << 17) | (h32 >> (32 - 17)));
    h32 = PRIME32_2*(h32^(h32 >> 15));
    h32 = PRIME32_3*(h32^(h32 >> 13));
    return h32^(h32 >> 16);
}

uint hash_xxhash32(uvec4 p)
{
    const uint PRIME32_2 = 2246822519U, PRIME32_3 = 3266489917U;
    const uint PRIME32_4 = 668265263U, PRIME32_5 = 374761393U;
    uint h32 =  p.w + PRIME32_5 + p.x*PRIME32_3;
    h32 = PRIME32_4*((h32 << 17) | (h32 >> (32 - 17)));
    h32 += p.y * PRIME32_3;
    h32 = PRIME32_4*((h32 << 17) | (h32 >> (32 - 17)));
    h32 += p.z * PRIME32_3;
    h32 = PRIME32_4*((h32 << 17) | (h32 >> (32 - 17)));
    h32 = PRIME32_2*(h32^(h32 >> 15));
    h32 = PRIME32_3*(h32^(h32 >> 13));
    return h32^(h32 >> 16);
}

uint spatialhash_cell_size(float cell_size_step, float min_cell_size)
{
    float log_step = floor(log2(cell_size_step / min_cell_size));
    return uint(min_cell_size * exp2(log_step));
}

ivec4 spatialhash_cell(vec3 world_pos, vec3 jitter, vec3 cam_pos, float dist_scale, float min_cell_size)
{
    /* Get coordinate + size for cell in spatial hash.
     * Attempt to avoid 'fragment' cells (only covering a relative small volume) by:
     * - snapping camera position to multiple of min_cell_size
     * - not using euclidean distance
     * - clamping distance to be in band for cell_size */
    cam_pos = floor((cam_pos / min_cell_size) + vec3(0.5)) * min_cell_size;
    vec3 d = world_pos - cam_pos;
    float len = max(abs(d.x), max(abs(d.y), abs(d.z)));
    uint cell_size = spatialhash_cell_size(len * dist_scale, min_cell_size);
    d = clamp(abs(d), cell_size, 2 * cell_size) * sign(d);
    return ivec4(ivec3(floor((world_pos + cell_size * jitter) / cell_size)), cell_size);
}

#endif // HASH_GLSL_

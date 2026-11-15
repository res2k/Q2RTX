/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2018 Tobias Zirr
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
Copyright (C) 2022 Jorge Gustavo Martinez

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

#ifndef  _RESTIR_H_
#define  _RESTIR_H_

#include "path_tracer_rgen.h"

#define RESTIR_INVALID_ID		0xFFFF

#define RESTIR_SPATIAL_DISTANCE 32
#define RESTIR_SPATIAL_SAMPLES  8

#define RESTIR_SAMPLING_M		4

// Packed reservoir layout (v2.2), 64 bits, RG32_UINT:
//   vec.x, bits  0..15 : light id y (RESTIR_INVALID_ID = 0xFFFF)
//   vec.x, bits 16..30 : W, fp16, clamped to RESTIR_W_MAX
//   vec.x, bit	 31 : vis_verified flag (fp16 sign bit; W >= 0, so it is free)
//   vec.y, bits  0..10 : y_pos.x, quantized to [0, 1]
//   vec.y, bits 11..21 : y_pos.y, quantized to [0, 1]
//   vec.y, bits 22..31 : M, real sample count, saturated at RESTIR_M_MAX
#define RESTIR_ID_MASK		 0xFFFFu
#define RESTIR_W_MAX		 65504.0 /* largest finite fp16 value */
// vis_verified rides in the sign bit of the fp16 W word in vec.x:
// W is provably >= 0 after sanitization, so the sign bit is free (res2k).
#define RESTIR_VIS_SHIFT	 31
// y_pos at 11 bits per component matches half-float mantissa precision
// for values in [0, 1] (res2k's standard).
#define RESTIR_YPOS_X_BITS	 11
#define RESTIR_YPOS_Y_BITS	 11
#define RESTIR_YPOS_X_MASK	 ((1u << RESTIR_YPOS_X_BITS) - 1u)
#define RESTIR_YPOS_Y_MASK	 ((1u << RESTIR_YPOS_Y_BITS) - 1u)
#define RESTIR_YPOS_X_LEVELS	 ((1u << RESTIR_YPOS_X_BITS) - 1u) /* quantization scale */
#define RESTIR_YPOS_Y_LEVELS	 ((1u << RESTIR_YPOS_Y_BITS) - 1u) /* quantization scale */
// M saturates at RESTIR_M_MAX at pack time (~250 frames of temporal
// compounding at 60 fps). The merge weights that matter are still
// capped by restir_m_clamp at combination time, so the saturation
// error stays bounded; see validation plan step V5.
#define RESTIR_M_BITS		 10
#define RESTIR_M_SHIFT		 (RESTIR_YPOS_X_BITS + RESTIR_YPOS_Y_BITS)
#define RESTIR_M_MAX		 ((1u << RESTIR_M_BITS) - 1u)

struct Reservoir
{
	// Persisted between frames
	uint y;	  // selected light
	float W;	 // weight of light
	vec2 y_pos;  // light sampling position

	// not persisted
	uint M;	  // number of sampled lights so far
	float w_sum; // sum of weights of lights so far
	float p_hat; // importance of selected light
	bool vis_verified; // whether shadow visibility was actually tested for this
};

void
init_reservoir(inout Reservoir r)
{
	r.y = RESTIR_INVALID_ID;
	r.M = 0;
	r.w_sum = 0.0;
	r.W = 0.0;
	r.p_hat = 0.0;
	r.y_pos = vec2(0.0);
	r.vis_verified = false;
}

bool
update_reservoir(uint xi, float wi, vec2 xi_pos, uint M, float p_hat, inout float rng, inout Reservoir r)
{
	r.w_sum += wi;
	r.M += M;
	float p_s = r.w_sum > 0.0 ? (wi/r.w_sum) : 0.0;
	if(rng < p_s)
	{
		r.y = xi;
		r.y_pos = xi_pos;
		r.p_hat = p_hat;
		rng /= p_s;
		return true;
	}
	else
	{
		rng = (rng - p_s) / (1.0f - p_s);
		return false;
	}
}

uvec4
pack_reservoir(Reservoir r, bool vis_verified)
{
	uvec4 vec;
	r.W = r.y == RESTIR_INVALID_ID ? 0.0 : r.W;
	// Sanitize before packing: NaN/Inf must not reach the fp16 word, and
	// W must be >= 0, because bit 31 of that word carries the
	// vis_verified flag below. W is provably >= 0; this is defensive.
	if(isnan(r.W) || isinf(r.W)) r.W = 0.0;
	if(r.W == 0.0) r.W = 0.0; /* normalize -0.0 to +0.0 */
	// Clamp to fp16 max: values above 65504 would become Inf in
	// packHalf2x16, and the isinf() guards in unpack_reservoir() would
	// silently drop the light -> black speckles in dim areas.
	r.W = clamp(r.W, 0.0, RESTIR_W_MAX);
	vec.x = packHalf2x16(vec2(r.W, r.W));
	vec.x = (vec.x & ~RESTIR_ID_MASK) | r.y;
	// W >= 0 always, so the fp16 sign bit is free: use it for vis_verified.
	vec.x |= (vis_verified ? 1u : 0u) << RESTIR_VIS_SHIFT;

	// Second u32, layout per the packing constants above:
	// y_pos quantized to [0, 1], real M.
	uint pos_qx = uint(clamp(r.y_pos.x, 0.0, 1.0) * float(RESTIR_YPOS_X_LEVELS) + 0.5);
	uint pos_qy = uint(clamp(r.y_pos.y, 0.0, 1.0) * float(RESTIR_YPOS_Y_LEVELS) + 0.5);
	uint m = (r.y == RESTIR_INVALID_ID) ? 0u : min(r.M, RESTIR_M_MAX);
	vec.y = pos_qx
		| (pos_qy << RESTIR_YPOS_X_BITS)
		| (m << RESTIR_M_SHIFT);
	return vec;
}

void
unpack_reservoir(uvec4 packed, out Reservoir r)
{
	r.y = packed.x & RESTIR_ID_MASK;
	r.vis_verified = ((packed.x >> RESTIR_VIS_SHIFT) & 1u) != 0;
	vec2 val = unpackHalf2x16(packed.x & ~(1u << RESTIR_VIS_SHIFT));
	r.W = val.y;
	if(isnan(r.W) || isinf(r.W)) r.W = 0.0;
	r.y_pos = vec2(float(packed.y & RESTIR_YPOS_X_MASK),
			float((packed.y >> RESTIR_YPOS_X_BITS) & RESTIR_YPOS_Y_MASK))
			/ vec2(float(RESTIR_YPOS_X_LEVELS), float(RESTIR_YPOS_Y_LEVELS));
	r.M = (r.y == RESTIR_INVALID_ID) ? 0u : min(packed.y >> RESTIR_M_SHIFT, RESTIR_M_MAX);
	r.p_hat = 0.0;
	r.w_sum = 0;
}

#if RESTIR_PACK_SELFTEST
/* Debug round-trip test for pack_reservoir/unpack_reservoir (validation
 * plan step V1). Returns the number of failed checks (0 = all passed).
 * Compiled out unless RESTIR_PACK_SELFTEST is defined; hook it up to a
 * debug trigger to execute it. */
uint
restir_pack_selftest()
{
	uint failures = 0;
	float pos_step = 1.0 / float(RESTIR_YPOS_Y_LEVELS);
	float pos_step_x = 1.0 / float(RESTIR_YPOS_X_LEVELS);
	Reservoir s, t;
	uvec4 packed;

	// 1) y_pos boundaries: 0, one step, half, last step, 1
	float px[5];
	px[0] = 0.0;
	px[1] = pos_step;
	px[2] = 0.5;
	px[3] = 1.0 - pos_step;
	px[4] = 1.0;
	for(int i = 0; i < 5; i++)
	{
		init_reservoir(s);
		s.y = 42; s.W = 1.0; s.y_pos = vec2(px[i], 1.0 - px[i]); s.M = 7;
		packed = pack_reservoir(s, false);
		unpack_reservoir(packed, t);
		if(t.y != 42 || t.M != 7 || t.vis_verified) failures++;
		if(abs(t.W - 1.0) > 1e-4) failures++;
		if(abs(t.y_pos.x - px[i]) > pos_step_x * 0.501) failures++;
		if(abs(t.y_pos.y - (1.0 - px[i])) > pos_step * 0.501) failures++;
	}

	// 2) out-of-range y_pos clamps to [0, 1]
	init_reservoir(s);
	s.y = 42; s.W = 1.0; s.y_pos = vec2(-0.25, 1.75); s.M = 3;
	packed = pack_reservoir(s, true);
	unpack_reservoir(packed, t);
	if(t.y_pos.x != 0.0 || t.y_pos.y != 1.0) failures++;

	// 3) W clamp: at limit stays, above clamps down
	init_reservoir(s);
	s.y = 42; s.y_pos = vec2(0.5); s.M = 3;
	s.W = RESTIR_W_MAX;
	packed = pack_reservoir(s, false);
	unpack_reservoir(packed, t);
	if(abs(t.W - RESTIR_W_MAX) > 1.0) failures++;
	s.W = 100000.0;
	packed = pack_reservoir(s, false);
	unpack_reservoir(packed, t);
	if(t.W != RESTIR_W_MAX) failures++;

	// 4) W = 0 with valid id round-trips
	s.W = 0.0;
	packed = pack_reservoir(s, false);
	unpack_reservoir(packed, t);
	if(t.y != 42 || t.W != 0.0 || t.M != 3) failures++;

	// 5) invalid id: W and M zeroed, flag still round-trips
	init_reservoir(s);
	s.y = RESTIR_INVALID_ID; s.W = 12.5; s.y_pos = vec2(0.5); s.M = 9;
	packed = pack_reservoir(s, true);
	unpack_reservoir(packed, t);
	if(t.y != RESTIR_INVALID_ID || t.W != 0.0 || t.M != 0u) failures++;
	if(!t.vis_verified) failures++;

	// 6) M extremes: 0, 1, mid, max, above-max clamps; flag varied
	uint mCases[5];
	mCases[0] = 0u;
	mCases[1] = 1u;
	mCases[2] = 512u;
	mCases[3] = RESTIR_M_MAX;
	mCases[4] = RESTIR_M_MAX + 5u;
	for(int i = 0; i < 5; i++)
	{
		init_reservoir(s);
		s.y = 7; s.W = 2.0; s.y_pos = vec2(0.25, 0.75); s.M = mCases[i];
		packed = pack_reservoir(s, i == 1);
		unpack_reservoir(packed, t);
		if(t.M != min(mCases[i], RESTIR_M_MAX)) failures++;
		if(t.vis_verified != (i == 1)) failures++;
	}

	// 7) flag survives with W = 0
	init_reservoir(s);
	s.y = 7; s.W = 0.0; s.y_pos = vec2(0.5); s.M = 2;
	packed = pack_reservoir(s, true);
	unpack_reservoir(packed, t);
	if(!t.vis_verified) failures++;

	// 8) raw bit pattern spot check (guards against accidental layout drift)
	init_reservoir(s);
	s.y = 42; s.W = 1.0; s.y_pos = vec2(1.0, 0.0); s.M = 1;
	packed = pack_reservoir(s, true);
	uint expect = RESTIR_YPOS_X_MASK
		| (1u << RESTIR_M_SHIFT);
	if(packed.y != expect) failures++;
	if((packed.x & RESTIR_ID_MASK) != 42u) failures++;
	if((packed.x & (1u << RESTIR_VIS_SHIFT)) == 0) failures++;
	packed = pack_reservoir(s, false);
	if(packed.y != expect) failures++;
	if((packed.x & (1u << RESTIR_VIS_SHIFT)) != 0) failures++;

	return failures;
}
#endif  /* RESTIR_PACK_SELFTEST */

// Functions

uint
get_light_current_idx(uint index)
{
	if (index < global_ubo.num_static_lights || index == RESTIR_INVALID_ID)
	{
		return index;
	}
	else
	{
		uint light_id_curr = instance_buffer.mlight_prev_to_current[index - global_ubo.num_static_lights];
		if(light_id_curr != ~0u) return light_id_curr + global_ubo.num_static_lights;
		else
		{
			return RESTIR_INVALID_ID;
		}
	}
}


float
get_unshadowed_path_contrib(
		uint light_idx,
		vec3 position,
		vec3 normal,
		vec3 view_direction,
		float phong_exp,
		float phong_scale,
		float phong_weight,
		vec2 rng)
{
	LightPolygon light = get_light_polygon(light_idx);

	vec3 position_light = vec3(0);
	vec3 light_normal;
	float pdfw;
	switch(uint(light.type))
	{
		case LIGHT_POLYGON:
			position_light = sample_projected_triangle(position, light.positions, rng, light_normal, pdfw);
			break;
		case LIGHT_SPHERE:
			position_light = sample_projected_sphere(position, light.positions, rng, light_normal, pdfw);
			break;
		case LIGHT_SPOT:
			position_light = sample_projected_spotlight(position, light.positions, rng, light_normal, pdfw);
			break;
	}

	float m = 0.0f;
	vec3 L = normalize(position_light - position);
	if(dot(L, normal) <= 0)
		pdfw = 0;

	if (pdfw > 0)
	{
		float LdotNL = max(0, -dot(light_normal, L));
		float spotlight = sqrt(LdotNL);
		float inv_pdfw = 1.0 / pdfw;

		m = inv_pdfw * spotlight;
	}

	float light_lum = luminance(light.color);

	// Apply light style scaling.
	light_lum *= light.light_style_scale;

	if(light_lum < 0 && global_ubo.environment_type == ENVIRONMENT_DYNAMIC)
	{
		// Set limits on sky luminance to avoid oversampling the sky in shadowed areas, or undersampling at dusk and dawn.
		// Note: the log -> linear conversion of the cvars happens on the CPU, in main.c
		m *= clamp(sun_color_ubo.sky_luminance, global_ubo.pt_min_log_sky_luminance, global_ubo.pt_max_log_sky_luminance);
	}
	else
		m *= abs(light_lum); // abs because sky lights have negative color

	return m;
}


bool
process_selected_light_restir(
		uint light_idx,
		vec2 light_position,
		float weight,
		vec3 position,
		vec3 normal,
		vec3 geo_normal,
		int shadow_cull_mask,
		vec3 view_direction,
		vec3 base_reflectivity,
		float specular_factor,
		float roughness,
		int surface_medium,
		bool enable_caustics,
		float direct_specular_weight,
		float phong_exp,
		float phong_scale,
		float phong_weight,
		bool check_vis,
		uint cluster_idx,
		out vec3 diffuse,
		out vec3 specular,
		out float vis)
{
	float polygonal_light_pdfw = 0;
	vec3 contrib_polygonal = vec3(0);
	vec3 L, pos_on_light_polygonal;
	bool polygonal_light_is_sky = false;
	diffuse = vec3(0);
	specular = vec3(0);
	vis = 1.0;

	LightPolygon light = get_light_polygon(light_idx);

	vec3 light_normal;

	switch(uint(light.type))
	{
		case LIGHT_POLYGON:
			pos_on_light_polygonal = sample_projected_triangle(position, light.positions, light_position , light_normal, polygonal_light_pdfw);
			break;
		case LIGHT_SPHERE:
			pos_on_light_polygonal = sample_projected_sphere(position, light.positions, light_position , light_normal, polygonal_light_pdfw);
			break;
		case LIGHT_SPOT:
			pos_on_light_polygonal = sample_projected_spotlight(position, light.positions, light_position , light_normal, polygonal_light_pdfw);
			break;
	}

	L = normalize(pos_on_light_polygonal - position);

	if(dot(L, geo_normal) <= 0)
		polygonal_light_pdfw = 0;

	if(polygonal_light_pdfw > 0){
		float LdotNL = max(0, -dot(light_normal, L));
		float spotlight = sqrt(LdotNL);
		float inv_pdfw = 1.0 / polygonal_light_pdfw;

		if(light.color.r >= 0)
		{
			contrib_polygonal = light.color * (inv_pdfw * spotlight * light.light_style_scale);
		}
		else
		{
			contrib_polygonal = env_map(L, true) * inv_pdfw * global_ubo.pt_env_scale;
			polygonal_light_is_sky = true;
		}
	}

	contrib_polygonal *= weight;

	float spec_polygonal = phong(normal, L, view_direction, phong_exp) * phong_scale;

	float l_polygonal  = luminance(abs(contrib_polygonal)) * mix(1, spec_polygonal, phong_weight);

	bool null_light = (l_polygonal == 0);

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, pos_on_light_polygonal, 0);

	if(check_vis) vis *= trace_shadow_ray(shadow_ray, null_light ? 0 : shadow_cull_mask);

	#ifdef ENABLE_SHADOW_CAUSTICS
		if(enable_caustics)
		{
			contrib_polygonal *= trace_caustic_ray(shadow_ray, surface_medium);
		}
	#endif

	if(null_light)
	{
		vis = 0.0f;
		return false;
	}

	vec3 radiance = vis * contrib_polygonal;

	if(direct_specular_weight > 0 && polygonal_light_is_sky && global_ubo.pt_specular_mis != 0)
	{
		// MIS with direct specular and indirect specular.
		// Only applied to sky lights, for two reasons:
		//  1) Non-sky lights are trimmed to match the light texture, and indirect rays don't see that;
		//  2) Non-sky lights are usually away from walls, so the direct sampling issue is not as pronounced.

		direct_specular_weight *= get_specular_sampled_lighting_weight(roughness,
			normal, -view_direction, L, polygonal_light_pdfw);
	}

	vec3 F = vec3(0);

	if(vis > 0 && direct_specular_weight > 0)
	{
		vec3 specular_brdf = GGX_times_NdotL(view_direction, L,
			normal, roughness, base_reflectivity, 0.0, specular_factor, F);
		specular = radiance * specular_brdf * direct_specular_weight;
	}

	float NdotL = max(0, dot(normal, L));

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);

	return true;
}


void
get_direct_illumination_restir(
	vec3 position,
	vec3 normal,
	uint cluster_idx,
	vec3 view_direction,
	float phong_exp,
	float phong_scale,
	float phong_weight,
	bool is_gradient,
	Reservoir prev_r,
	out Reservoir reservoir)
{
	init_reservoir(reservoir);

	if(cluster_idx == ~0u)
		return;

	vec3 contrib_polygonal = vec3(0);

	float rng, p_hat;

	uint list_start = light_buffer.light_list_offsets[cluster_idx];
	uint list_end   = light_buffer.light_list_offsets[cluster_idx + 1];

	rng = get_rng(RNG_NEE_LIGHT_SELECTION(0));

	float list_size = float(list_end - list_start);
	float partitions = ceil(list_size / float(RESTIR_SAMPLING_M));
	float inv_pdf = list_size;
	float rng_part = rng * partitions;
	float fpart = min(floor(rng_part), partitions-1);

	list_start += int(fpart);
	int stride = int(partitions);
	rng = rng_part - floor(rng_part);

	uint current_light_idx;

	vec2 rng2 = vec2(
		get_rng(RNG_NEE_TRI_X(0)),
		get_rng(RNG_NEE_TRI_Y(0)));

	#pragma unroll
	for(uint i = 0, n_idx = list_start; i < RESTIR_SAMPLING_M; i++, n_idx += stride)
	{
		if (n_idx >= list_end)
			break;

		current_light_idx = light_buffer.light_list_lights[n_idx];

		if(current_light_idx == ~0u) continue;

		p_hat = get_unshadowed_path_contrib(current_light_idx, position, normal, view_direction, phong_exp, phong_scale, phong_weight, rng2);

		// Apply CDF adjustment based on light shadowing statistics from one of the previous frames.
		// See comments in function `get_direct_illumination` in `path_tracer_rgen.h`
		if(global_ubo.pt_light_stats != 0 
			&& p_hat > 0 
			&& current_light_idx < global_ubo.num_static_lights)
		{
			uint buffer_idx = global_ubo.current_frame_idx;
			// Regular pixels get shadowing stats from the previous frame;
			// Gradient pixels get the stats from two frames ago because they need to match
			// the light sampling from the previous frame.
			buffer_idx += is_gradient ? (NUM_LIGHT_STATS_BUFFERS - 2) : (NUM_LIGHT_STATS_BUFFERS - 1);
			buffer_idx = buffer_idx % NUM_LIGHT_STATS_BUFFERS;

			uint addr = get_light_stats_addr(cluster_idx, current_light_idx, get_primary_direction(normal));

			uint num_hits = light_stats_bufers[buffer_idx].stats[addr];
			uint num_misses = light_stats_bufers[buffer_idx].stats[addr + 1];
			uint num_total = num_hits + num_misses;

			if(num_total > 0)
			{
				// Adjust the mass, but set a lower limit on the factor to avoid
				// extreme changes in the sampling.
				p_hat *= max(float(num_hits) / float(num_total), 0.1);
			}
		}

		// Add sample even if 0, so M will be correct
		update_reservoir(current_light_idx, p_hat * inv_pdf, rng2, 1, p_hat, rng, reservoir);
	}

	//Combine with temporal
	// Reservoirs with W == 0 (the light turned out to be shadowed there last
	// frame) carry no weight; merging them would still add their M to the
	// denominator of W = w_sum / (p_hat * M) and dim the new estimate.
	if(prev_r.y != RESTIR_INVALID_ID && prev_r.W > 0.0 && prev_r.vis_verified)
	{
		// Credit at most restir_m_clamp samples from the previous frame.
		uint prev_M = min(prev_r.M, uint(global_ubo.restir_m_clamp));
		update_reservoir(prev_r.y, prev_r.p_hat * prev_r.W * prev_M, prev_r.y_pos, prev_M, prev_r.p_hat, rng, reservoir);
	}

	reservoir.W = reservoir.w_sum / (reservoir.p_hat * reservoir.M);
	if(isnan(reservoir.W) || isinf(reservoir.W)) reservoir.W = 0.0;
}

#endif  /*_RESTIR_H_*/

// SPDX-License-Identifier: Apache-2.0
// ----------------------------------------------------------------------------
// Copyright 2011-2021 Arm Limited
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy
// of the License at:
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
// ----------------------------------------------------------------------------

#if !defined(ASTCENC_DECOMPRESS_ONLY)

/**
 * @brief Functions for finding best partition for a block.
 *
 * Major step 1:
 * - find best partitioning assuming uncorrelated colors
 * - find best partitioning assuming RGBS color representation
 *
 * Finding best partitioning for a block:
 *
 * foreach available partitioning:
 * - compute mean-color-value and dominant direction.
 * - this defines two lines, both of which go through the mean-color-value.
 * - one line has a direction defined by the dominant direction; this is used
 *   to assess the error from using an uncorrelated color representation.
 * - the other line goes through (0,0,0,1) and is used to assess the error from
 *   using an RGBS color representation.
 * - we then compute, as a sum across the block, the squared-errors that result
 *   from using the dominant-direction-lines and the squared-errors that result
 *   from using the 0001-lines.
 *
 *	Partition table representation:
 *	We have 3 tables, each with 1024 partitions
 *	(these correspond to the 3x128 hardware partitions crossed with all the
 *	partition-transform modes in the hardware.)
 *
 *	For each partitioning, we have:
 *	* a 4-entry table indicating how many texels there are in each of the 4
 *	  partitions. this may be from 2 to about 60 or so.
 *	* a 64-entry table indicating the partition index of each of the 64 texels
 *	  in the block. each index may be 0, 1, 2 or 3.
 *
 * each element in the table is an uint8_t indicating partition index (0, 1, 2 or 3)
 */

#include "astcenc_internal.h"

static void compute_partition_error_color_weightings_and_range(
	const imageblock* blk,
	const error_weight_block* ewb,
	const partition_info* pt,
	vfloat4 error_weightings[4],
	vfloat4 color_scale_factors[4],
	vfloat4 ranges_rgba[4]
) {
	int partition_count = pt->partition_count;

	for (int i = 0; i < partition_count; i++)
	{
		vfloat4 error_weight(1e-12f);
		vfloat4 rgba_min(1e38f);
		vfloat4 rgba_max(-1e38f);

		int texel_count = pt->texels_per_partition[i];
		for (int j = 0; j < texel_count; j++)
		{
			int tidx = pt->texels_of_partition[i][j];
			error_weight = error_weight + ewb->error_weights[tidx];

			if (ewb->texel_weight[tidx] > 1e-10f)
			{
				vfloat4 data = blk->texel(tidx);
				rgba_min = min(data, rgba_min);
				rgba_max = max(data, rgba_max);
			}
		}

		error_weight = error_weight / pt->texels_per_partition[i];
		error_weightings[i] = error_weight;
		color_scale_factors[i] = sqrt(error_weight);
		ranges_rgba[i] = max(rgba_max - rgba_min, 1e-10f);
	}
}


void compute_partition_error_color_weightings(
	const block_size_descriptor* bsd,
	const error_weight_block* ewb,
	const partition_info* pt,
	vfloat4 error_weightings[4],
	vfloat4 color_scale_factors[4]
) {
	int texels_per_block = bsd->texel_count;
	int pcnt = pt->partition_count;

	for (int i = 0; i < pcnt; i++)
	{
		error_weightings[i] = vfloat4(1e-12f);
	}

	for (int i = 0; i < texels_per_block; i++)
	{
		int part = pt->partition_of_texel[i];
		error_weightings[part] = error_weightings[part] + ewb->error_weights[i];
	}

	for (int i = 0; i < pcnt; i++)
	{
		error_weightings[i] = error_weightings[i] * (1.0f / pt->texels_per_partition[i]);
		color_scale_factors[i] = sqrt(error_weightings[i]);
	}
}

/* main function to identify the best partitioning for a given number of texels */
void find_best_partitionings(
	const block_size_descriptor* bsd,
	const imageblock* blk,
	const error_weight_block* ewb,
	int partition_count,
	int partition_search_limit,
	int* best_partition_uncor,
	int* best_partition_samec,
	int* best_partition_dualplane
) {
	// constant used to estimate quantization error for a given partitioning;
	// the optimal value for this constant depends on bitrate.
	// These constants have been determined empirically.
	int texels_per_block = bsd->texel_count;
	float weight_imprecision_estim = 0.055f;
	if (texels_per_block <= 20)
	{
		weight_imprecision_estim = 0.03f;
	}
	else if (texels_per_block <= 31)
	{
		weight_imprecision_estim = 0.04f;
	}
	else if (texels_per_block <= 41)
	{
		weight_imprecision_estim = 0.05f;
	}

	weight_imprecision_estim = weight_imprecision_estim * weight_imprecision_estim;

	int partition_sequence[PARTITION_COUNT];

	kmeans_compute_partition_ordering(bsd, partition_count, blk, partition_sequence);

	int uses_alpha = imageblock_uses_alpha(blk);

	const partition_info* ptab = get_partition_table(bsd, partition_count);

	// Partitioning errors assuming uncorrelated-chrominance endpoints
	float uncor_best_error { ERROR_CALC_DEFAULT };
	int uncor_best_partition { 0 };

	// Partitioning errors assuming same-chrominance endpoints
	// Store two so we can always return one different to uncorr
	float samec_best_errors[2] { ERROR_CALC_DEFAULT, ERROR_CALC_DEFAULT };
	int samec_best_partitions[2] { 0, 0 };

	// Partitioning errors assuming that one color component is uncorrelated
	float sep_best_error { ERROR_CALC_DEFAULT };
	int sep_best_partition { 0 };
	int sep_best_component { 0 };

	if (uses_alpha)
	{

		for (int i = 0; i < partition_search_limit; i++)
		{
			int partition = partition_sequence[i];

			// TODO: Can we avoid returning these completely?
			int bk_partition_count = ptab[partition].partition_count;
			if (bk_partition_count < partition_count)
			{
				continue;
			}

			// compute the weighting to give to each color channel
			// in each partition.
			vfloat4 error_weightings[4];
			vfloat4 color_scale_factors[4];
			vfloat4 icolor_scale_factors[4];
			vfloat4 ranges_rgba[4];

			compute_partition_error_color_weightings_and_range(
			    blk, ewb, ptab + partition, error_weightings,
			    color_scale_factors, ranges_rgba);

			for (int j = 0; j < partition_count; j++)
			{
				icolor_scale_factors[j] = 1.0f / max(color_scale_factors[j], 1e-7f);
			}

			vfloat4 avgs_rgba[4];
			vfloat4 dirs_rgba[4];

			compute_avgs_and_dirs_4_comp(ptab + partition, blk, ewb,
			                             color_scale_factors, avgs_rgba,
			                             dirs_rgba);

			line4 uncor_lines[4];
			line4 samec_lines[4];
			line3 sep_r_lines[4];
			line3 sep_g_lines[4];
			line3 sep_b_lines[4];
			line3 sep_a_lines[4];

			processed_line4 uncor_plines[4];
			processed_line4 samec_plines[4];
			processed_line3 sep_r_plines[4];
			processed_line3 sep_g_plines[4];
			processed_line3 sep_b_plines[4];
			processed_line3 sep_a_plines[4];

			float uncor_line_lens[4];
			float samec_line_lens[4];

			for (int j = 0; j < partition_count; j++)
			{
				uncor_lines[j].a = avgs_rgba[j];
				if (dot_s(dirs_rgba[j], dirs_rgba[j]) == 0.0f)
				{
					uncor_lines[j].b = normalize(vfloat4(1.0f));
				}
				else
				{
					uncor_lines[j].b = normalize(dirs_rgba[j]);
				}

				uncor_plines[j].amod = (uncor_lines[j].a - uncor_lines[j].b * dot(uncor_lines[j].a, uncor_lines[j].b)) * icolor_scale_factors[j];
				uncor_plines[j].bs = uncor_lines[j].b * color_scale_factors[j];
				uncor_plines[j].bis = uncor_lines[j].b * icolor_scale_factors[j];

				samec_lines[j].a = vfloat4::zero();
				if (dot_s(avgs_rgba[j], avgs_rgba[j]) == 0.0f)
				{
					samec_lines[j].b = normalize(vfloat4(1.0f));
				}
				else
				{
					samec_lines[j].b = normalize(avgs_rgba[j]);
				}

				samec_plines[j].amod = (samec_lines[j].a - samec_lines[j].b * dot(samec_lines[j].a, samec_lines[j].b)) * icolor_scale_factors[j];
				samec_plines[j].bs = samec_lines[j].b * color_scale_factors[j];
				samec_plines[j].bis = samec_lines[j].b * icolor_scale_factors[j];

				sep_r_lines[j].a = avgs_rgba[j].swz<1, 2, 3>();
				vfloat4 dirs_gba = dirs_rgba[j].swz<1, 2, 3, 0>();
				if (dot3_s(dirs_gba, dirs_gba) == 0.0f)
				{
					sep_r_lines[j].b = normalize(vfloat4(1.0f, 1.0f, 1.0f, 0.0f));
				}
				else
				{
					sep_r_lines[j].b = normalize(dirs_gba);
				}

				sep_g_lines[j].a = avgs_rgba[j].swz<0, 2, 3>();
				vfloat4 dirs_rba = dirs_rgba[j].swz<0, 2, 3, 1>();
				if (dot3_s(dirs_rba, dirs_rba) == 0.0f)
				{
					sep_g_lines[j].b = normalize(vfloat4(1.0f, 1.0f, 1.0f, 0.0f));
				}
				else
				{
					sep_g_lines[j].b = normalize(dirs_rba);
				}

				sep_b_lines[j].a = avgs_rgba[j].swz<0, 1, 3>();
				vfloat4 dirs_rga = dirs_rgba[j].swz<0, 1, 3, 2>();
				if (dot3_s(dirs_rga, dirs_rga) == 0.0f)
				{
					sep_b_lines[j].b = normalize(vfloat4(1.0f, 1.0f, 1.0f, 0.0f));
				}
				else
				{
					sep_b_lines[j].b = normalize(dirs_rga);
				}

				sep_a_lines[j].a = avgs_rgba[j].swz<0, 1, 2>();
				vfloat4 dirs_rgb = dirs_rgba[j].swz<0, 1, 2, 3>();
				if (dot3_s(dirs_rgb, dirs_rgb) == 0.0f)
				{
					sep_a_lines[j].b = normalize(vfloat4(1.0f, 1.0f, 1.0f, 0.0f));
				}
				else
				{
					sep_a_lines[j].b = normalize(dirs_rgb);
				}

				sep_r_plines[j].amod = (sep_r_lines[j].a - sep_r_lines[j].b * dot3(sep_r_lines[j].a, sep_r_lines[j].b)) * icolor_scale_factors[j].swz<1, 2, 3, 0>();
				sep_r_plines[j].bs   = (sep_r_lines[j].b * color_scale_factors[j].swz<1, 2, 3, 0>());
				sep_r_plines[j].bis  = (sep_r_lines[j].b * icolor_scale_factors[j].swz<1, 2, 3, 0>());

				sep_g_plines[j].amod = (sep_g_lines[j].a - sep_g_lines[j].b * dot3(sep_g_lines[j].a, sep_g_lines[j].b)) * icolor_scale_factors[j].swz<0, 2, 3, 1>();
				sep_g_plines[j].bs   = (sep_g_lines[j].b * color_scale_factors[j].swz<0, 2, 3, 1>());
				sep_g_plines[j].bis  = (sep_g_lines[j].b * icolor_scale_factors[j].swz<0, 2, 3, 1>());

				sep_b_plines[j].amod = (sep_b_lines[j].a - sep_b_lines[j].b * dot3(sep_b_lines[j].a, sep_b_lines[j].b)) * icolor_scale_factors[j].swz<0, 1, 3, 2>();
				sep_b_plines[j].bs   = (sep_b_lines[j].b * color_scale_factors[j].swz<0, 1, 3, 2>());
				sep_b_plines[j].bis  = (sep_b_lines[j].b * icolor_scale_factors[j].swz<0, 1, 3, 2>());

				sep_a_plines[j].amod = (sep_a_lines[j].a - sep_a_lines[j].b * dot3(sep_a_lines[j].a, sep_a_lines[j].b)) * icolor_scale_factors[j].swz<0, 1, 2, 3>();
				sep_a_plines[j].bs   = (sep_a_lines[j].b * color_scale_factors[j].swz<0, 1, 2, 3>());
				sep_a_plines[j].bis  = (sep_a_lines[j].b * icolor_scale_factors[j].swz<0, 1, 2, 3>());
			}

			float uncor_error = 0.0f;
			float samec_error = 0.0f;
			// TODO: Try this as a scalar
			vfloat4 sep_error = vfloat4::zero();
			compute_error_squared_rgba(ptab + partition,
			                           blk,
			                           ewb,
			                           uncor_plines,
			                           samec_plines,
			                           uncor_line_lens,
			                           samec_line_lens,
			                           &uncor_error,
			                           &samec_error);

			/*
			   Compute an estimate of error introduced by weight quantization imprecision.
			   This error is computed as follows, for each partition
			   1: compute the principal-axis vector (full length) in error-space
			   2: convert the principal-axis vector to regular RGB-space
			   3: scale the vector by a constant that estimates average quantization error
			   4: for each texel, square the vector, then do a dot-product with the texel's error weight;
			      sum up the results across all texels.
			   4(optimized): square the vector once, then do a dot-product with the average texel error,
			      then multiply by the number of texels.
			 */

			for (int j = 0; j < partition_count; j++)
			{
				float tpp = (float)(ptab[partition].texels_per_partition[j]);

				vfloat4 ics = icolor_scale_factors[j];
				vfloat4 error_weights = error_weightings[j] * (tpp * weight_imprecision_estim);

				vfloat4 uncor_vector = uncor_lines[j].b * uncor_line_lens[j] * ics;
				vfloat4 samec_vector = samec_lines[j].b * samec_line_lens[j] * ics;
				vfloat4 sep_r_vector = sep_r_lines[j].b * ics.swz<1, 2, 3, 0>();
				vfloat4 sep_g_vector = sep_g_lines[j].b * ics.swz<0, 2, 3, 1>();
				vfloat4 sep_b_vector = sep_b_lines[j].b * ics.swz<0, 1, 3, 2>();
				vfloat4 sep_a_vector = sep_a_lines[j].b * ics.swz<0, 1, 2, 3>();

				uncor_vector = uncor_vector * uncor_vector;
				samec_vector = samec_vector * samec_vector;
				sep_r_vector = sep_r_vector * sep_r_vector;
				sep_g_vector = sep_g_vector * sep_g_vector;
				sep_b_vector = sep_b_vector * sep_b_vector;
				sep_a_vector = sep_a_vector * sep_a_vector;

				uncor_error += dot_s(uncor_vector, error_weights);
				samec_error += dot_s(samec_vector, error_weights);

				vfloat4 sep_err_inc(dot3_s(sep_r_vector, error_weights.swz<1, 2, 3, 0>()),
				                    dot3_s(sep_g_vector, error_weights.swz<0, 2, 3, 1>()),
				                    dot3_s(sep_b_vector, error_weights.swz<0, 1, 3, 2>()),
				                    dot3_s(sep_a_vector, error_weights.swz<0, 1, 2, 3>()));

				sep_error = sep_error + sep_err_inc + (ranges_rgba[j] * ranges_rgba[j] * error_weights);
			}

			if (uncor_error < uncor_best_error)
			{
				uncor_best_error = uncor_error;
				uncor_best_partition = partition;
			}

			if (samec_error < samec_best_errors[0])
			{
				samec_best_errors[1] = samec_best_errors[0];
				samec_best_partitions[1] = samec_best_partitions[0];

				samec_best_errors[0] = samec_error;
				samec_best_partitions[0] = partition;
			}
			else if (samec_error < samec_best_errors[1])
			{
				samec_best_errors[1] = samec_error;
				samec_best_partitions[1] = partition;
			}

			if (sep_error.lane<0>() < sep_best_error)
			{
				sep_best_error = sep_error.lane<0>();
				sep_best_partition = partition;
				sep_best_component = 0;
			}

			if (sep_error.lane<1>() < sep_best_error)
			{
				sep_best_error = sep_error.lane<1>();
				sep_best_partition = partition;
				sep_best_component = 1;
			}

			if (sep_error.lane<2>() < sep_best_error)
			{
				sep_best_error = sep_error.lane<2>();
				sep_best_partition = partition;
				sep_best_component = 2;
			}

			if (sep_error.lane<3>() < sep_best_error)
			{
				sep_best_error = sep_error.lane<3>();
				sep_best_partition = partition;
				sep_best_component = 3;
			}
		}
	}
	else
	{
		for (int i = 0; i < partition_search_limit; i++)
		{
			int partition = partition_sequence[i];

			int bk_partition_count = ptab[partition].partition_count;
			if (bk_partition_count < partition_count)
			{
				continue;
			}

			// compute the weighting to give to each color channel
			// in each partition.
			vfloat4 error_weightings[4];
			vfloat4 color_scale_factors[4];
			vfloat4 icolor_scale_factors[4];
			vfloat4 ranges_rgba[4];
			compute_partition_error_color_weightings_and_range(
			    blk, ewb, ptab + partition, error_weightings,
			    color_scale_factors, ranges_rgba);

			for (int j = 0; j < partition_count; j++)
			{
				icolor_scale_factors[j] = 1.0f / max(color_scale_factors[j], 1e-7f);
			}

			vfloat4 avgs_rgba[4];
			vfloat4 dirs_rgb[4];

			compute_avgs_and_dirs_3_comp(ptab + partition, blk, ewb, color_scale_factors, 3, avgs_rgba, dirs_rgb);

			line3 uncor_lines[4];
			line3 samec_lines[4];
			line2 sep_r_lines[4];
			line2 sep_g_lines[4];
			line2 sep_b_lines[4];

			processed_line3 uncor_plines[4];
			processed_line3 samec_plines[4];

			processed_line2 sep_r_plines[4];
			processed_line2 sep_g_plines[4];
			processed_line2 sep_b_plines[4];

			float uncor_line_lens[4];
			float samec_line_lens[4];

			for (int j = 0; j < partition_count; j++)
			{
				uncor_lines[j].a = avgs_rgba[j];
				if (dot3_s(dirs_rgb[j], dirs_rgb[j]) == 0.0f)
				{
					uncor_lines[j].b = normalize(vfloat4(1.0f, 1.0f, 1.0f, 0.0f));
				}
				else
				{
					uncor_lines[j].b = normalize(dirs_rgb[j]);
				}

				samec_lines[j].a = vfloat4::zero();
				if (dot3_s(avgs_rgba[j], avgs_rgba[j]) == 0.0f)
				{
					samec_lines[j].b = normalize(vfloat4(1.0f, 1.0f, 1.0f, 0.0f));
				}
				else
				{
					samec_lines[j].b = normalize(avgs_rgba[j]);
				}

				uncor_plines[j].amod = (uncor_lines[j].a - uncor_lines[j].b * dot3(uncor_lines[j].a, uncor_lines[j].b)) * icolor_scale_factors[j].swz<0, 1, 2, 3>();
				uncor_plines[j].bs   = (uncor_lines[j].b * color_scale_factors[j].swz<0, 1, 2, 3>());
				uncor_plines[j].bis  = (uncor_lines[j].b * icolor_scale_factors[j].swz<0, 1, 2, 3>());

				samec_plines[j].amod = (samec_lines[j].a - samec_lines[j].b * dot(samec_lines[j].a, samec_lines[j].b)) * icolor_scale_factors[j].swz<0, 1, 2, 3>();
				samec_plines[j].bs   = (samec_lines[j].b * color_scale_factors[j].swz<0, 1, 2, 3>());
				samec_plines[j].bis  = (samec_lines[j].b * icolor_scale_factors[j].swz<0, 1, 2, 3>());

				sep_r_lines[j].a = avgs_rgba[j].swz<1, 2>();
				float2 dirs_gb = dirs_rgb[j].swz<1, 2>();
				if (dot(dirs_gb, dirs_gb) == 0.0f)
				{
					sep_r_lines[j].b = normalize(float2(1.0f));
				}
				else
				{
					sep_r_lines[j].b = normalize(dirs_gb);
				}

				sep_g_lines[j].a = avgs_rgba[j].swz<0, 2>();
				float2 dirs_rb = dirs_rgb[j].swz<0, 2>();
				if (dot(dirs_rb, dirs_rb) == 0.0f)
				{
					sep_g_lines[j].b = normalize(float2(1.0f));
				}
				else
				{
					sep_g_lines[j].b = normalize(dirs_rb);
				}

				sep_b_lines[j].a = avgs_rgba[j].swz<0, 1>();
				float2 dirs_rg = dirs_rgb[j].swz<0, 1>();
				if (dot(dirs_rg, dirs_rg) == 0.0f)
				{
					sep_b_lines[j].b = normalize(float2(1.0f));
				}
				else
				{
					sep_b_lines[j].b = normalize(dirs_rg);
				}

				sep_r_plines[j].amod = (sep_r_lines[j].a - sep_r_lines[j].b * dot(sep_r_lines[j].a, sep_r_lines[j].b)) * icolor_scale_factors[j].swz<1, 2>();
				sep_r_plines[j].bs   = (sep_r_lines[j].b * color_scale_factors[j].swz<1, 2>());
				sep_r_plines[j].bis  = (sep_r_lines[j].b *  icolor_scale_factors[j].swz<1, 2>());

				sep_g_plines[j].amod = (sep_g_lines[j].a - sep_g_lines[j].b * dot(sep_g_lines[j].a, sep_g_lines[j].b)) *  icolor_scale_factors[j].swz<0, 2>();
				sep_g_plines[j].bs   = (sep_g_lines[j].b * color_scale_factors[j].swz<0, 2>());
				sep_g_plines[j].bis  = (sep_g_lines[j].b *  icolor_scale_factors[j].swz<0, 2>());

				sep_b_plines[j].amod = (sep_b_lines[j].a - sep_b_lines[j].b * dot(sep_b_lines[j].a, sep_b_lines[j].b)) *  icolor_scale_factors[j].swz<0, 1>();
				sep_b_plines[j].bs   = (sep_b_lines[j].b * color_scale_factors[j].swz<0, 1>());
				sep_b_plines[j].bis  = (sep_b_lines[j].b *  icolor_scale_factors[j].swz<0, 1>());
			}

			float uncor_error = 0.0f;
			float samec_error = 0.0f;
			vfloat4 sep_error = vfloat4(0.0f);

			compute_error_squared_rgb(ptab + partition,
			                          blk,
			                          ewb,
			                          uncor_plines,
			                          samec_plines,
			                          uncor_line_lens,
			                          samec_line_lens,
			                          &uncor_error,
			                          &samec_error);

			/*
			   compute an estimate of error introduced by weight imprecision.
			   This error is computed as follows, for each partition
			   1: compute the principal-axis vector (full length) in error-space
			   2: convert the principal-axis vector to regular RGB-space
			   3: scale the vector by a constant that estimates average quantization error.
			   4: for each texel, square the vector, then do a dot-product with the texel's error weight;
			      sum up the results across all texels.
			   4(optimized): square the vector once, then do a dot-product with the average texel error,
			      then multiply by the number of texels.
			 */

			for (int j = 0; j < partition_count; j++)
			{
				float tpp = (float)(ptab[partition].texels_per_partition[j]);

				vfloat4 ics = icolor_scale_factors[j];
				ics.set_lane<3>(0.0f);

				vfloat4 error_weights = error_weightings[j] * (tpp * weight_imprecision_estim);
				error_weights.set_lane<3>(0.0f);

				vfloat4 uncor_vector = (uncor_lines[j].b * uncor_line_lens[j]) * ics;
				vfloat4 samec_vector = (samec_lines[j].b * samec_line_lens[j]) * ics;

				float2 sep_r_vector = sep_r_lines[j].b * ics.swz<1, 2>();
				float2 sep_g_vector = sep_g_lines[j].b * ics.swz<0, 2>();
				float2 sep_b_vector = sep_b_lines[j].b * ics.swz<0, 1>();

				uncor_vector = uncor_vector * uncor_vector;
				samec_vector = samec_vector * samec_vector;
				sep_r_vector = sep_r_vector * sep_r_vector;
				sep_g_vector = sep_g_vector * sep_g_vector;
				sep_b_vector = sep_b_vector * sep_b_vector;

				uncor_error += dot3_s(uncor_vector, error_weights);
				samec_error += dot3_s(samec_vector, error_weights);
				sep_error.set_lane<0>(sep_error.lane<0>() + dot(sep_r_vector, error_weights.swz<1, 2>()));
				sep_error.set_lane<1>(sep_error.lane<1>() + dot(sep_g_vector, error_weights.swz<0, 2>()));
				sep_error.set_lane<2>(sep_error.lane<2>() + dot(sep_b_vector, error_weights.swz<0, 1>()));

				sep_error.set_lane<0>(sep_error.lane<0>() + ranges_rgba[j].lane<0>() * ranges_rgba[j].lane<0>() * error_weights.lane<0>());
				sep_error.set_lane<1>(sep_error.lane<1>() + ranges_rgba[j].lane<1>() * ranges_rgba[j].lane<1>() * error_weights.lane<1>());
				sep_error.set_lane<2>(sep_error.lane<2>() + ranges_rgba[j].lane<2>() * ranges_rgba[j].lane<2>() * error_weights.lane<2>());
			}

			if (uncor_error < uncor_best_error)
			{
				uncor_best_error = uncor_error;
				uncor_best_partition = partition;
			}

			if (samec_error < samec_best_errors[0])
			{
				samec_best_errors[1] = samec_best_errors[0];
				samec_best_partitions[1] = samec_best_partitions[0];

				samec_best_errors[0] = samec_error;
				samec_best_partitions[0] = partition;
			}
			else if (samec_error < samec_best_errors[1])
			{
				samec_best_errors[1] = samec_error;
				samec_best_partitions[1] = partition;
			}

			if (sep_error.lane<0>() < sep_best_error)
			{
				sep_best_error = sep_error.lane<0>();
				sep_best_partition = partition;
				sep_best_component = 0;
			}

			if (sep_error.lane<1>() < sep_best_error)
			{
				sep_best_error = sep_error.lane<1>();
				sep_best_partition = partition;
				sep_best_component = 1;
			}

			if (sep_error.lane<2>() < sep_best_error)
			{
				sep_best_error = sep_error.lane<2>();
				sep_best_partition = partition;
				sep_best_component = 2;
			}
		}
	}

	*best_partition_uncor = uncor_best_partition;

	int index = samec_best_partitions[0] != uncor_best_partition ? 0 : 1;
	*best_partition_samec = samec_best_partitions[index];

	*best_partition_dualplane = (sep_best_component << PARTITION_BITS) |
	                            (sep_best_partition);
}

#endif

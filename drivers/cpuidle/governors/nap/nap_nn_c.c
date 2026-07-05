// SPDX-License-Identifier: GPL-2.0
/*
 * nap_nn_c.c — Plain C forward pass and backpropagation for the nap MLP.
 *
 * 8→8 trunk + scalar score s feeding the ordinal survival head.
 * Compiled with CFLAGS += -ffreestanding, allowing GCC/Clang to auto-vectorize
 * using NEON instructions on ARM64.
 *
 * Must be called within kernel_neon_begin/end blocks.
 */

#include "nap.h"

static inline float fclampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

void nap_nn_forward_c(const float *input,
		      float *output,
		      float *hidden_save,
		      const struct nap_weights *w)
{
	int i, j;
	float sum;

	/* === Hidden layer: 8 outputs === */
	for (i = 0; i < NAP_HIDDEN_SIZE; i++) {
		float acc = w->b_h1[i];
		for (j = 0; j < NAP_INPUT_SIZE; j++) {
			acc += w->w_h1[j][i] * input[j];
		}
		/* ReLU */
		hidden_save[i] = acc > 0.0f ? acc : 0.0f;
	}

	/* === Output layer === */
	sum = w->b_out;
	for (i = 0; i < NAP_HIDDEN_SIZE; i++) {
		sum += w->w_out[i] * hidden_save[i];
	}
	*output = sum;
}

void nap_nn_learn_c(struct nap_cpu_data *d)
{
	int i, j;
	float d_out_scalar = d->learn_d_out;
	float *d_hid = d->learn_d_hid;
	float lr = d->learn_lr;
	float clamp_val = (float)d->max_grad_norm_millths / 1000.0f;
	float grad, feat;

	/* Hidden gradient: d_hid[j] = relu'(h[j]) * w_out[j] * d_out */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++) {
		if (d->hidden_out[j] > 0.0f) {
			d_hid[j] = d->active_w->w_out[j] * d_out_scalar;
		} else {
			d_hid[j] = 0.0f;
		}
	}

	/* Output weight update: w_out[j] -= lr * clamp(h[j] * d_out) */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++) {
		grad = d->hidden_out[j] * d_out_scalar;
		d->active_w->w_out[j] -= lr * fclampf(grad, -clamp_val, clamp_val);
	}

	/* Output bias update */
	d->active_w->b_out -= lr * fclampf(d_out_scalar, -clamp_val, clamp_val);

	/* Hidden weight update: w_h1[i][j] -= lr * clamp(feat[i] * d_hid[j]) */
	for (i = 0; i < NAP_INPUT_SIZE; i++) {
		feat = d->features_f32[i];
		for (j = 0; j < NAP_HIDDEN_SIZE; j++) {
			grad = feat * d_hid[j];
			d->active_w->w_h1[i][j] -= lr * fclampf(grad, -clamp_val, clamp_val);
		}
	}

	/* Hidden bias update: b_h1[j] -= lr * clamp(d_hid[j]) */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++) {
		d->active_w->b_h1[j] -= lr * fclampf(d_hid[j], -clamp_val, clamp_val);
	}
}

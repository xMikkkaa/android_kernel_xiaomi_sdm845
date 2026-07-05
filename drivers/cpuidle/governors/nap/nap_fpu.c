// SPDX-License-Identifier: GPL-2.0
/*
 * nap_fpu.c — FPU/SIMD code for the NAP cpuidle governor.
 *
 * Adapted for ARM64 NEON.
 * Compiled with CFLAGS += -ffreestanding, allowing floating point and NEON.
 */

#include <linux/cpuidle.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/pm_qos.h>
#include <linux/string.h>
#include <linux/tick.h>
#include "nap.h"

#define PM_QOS_LATENCY_ANY_NS  ((s64)PM_QOS_LATENCY_ANY * NSEC_PER_USEC)

/* ================================================================
 * Float math helpers
 * ================================================================ */

static inline float float_min(float a, float b) { return a < b ? a : b; }
static inline float float_max(float a, float b) { return a > b ? a : b; }

static inline float fclampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static inline float nap_sqrtf(float x)
{
	float y;
	union { float f; u32 i; } u;

	if (x <= 0.0f)
		return 0.0f;

	u.f = x;
	u.i = 0x1fbc5df7 + (u.i >> 1);
	y = u.f;

	/* 2 iterations of Newton-Raphson */
	y = 0.5f * (y + x / y);
	y = 0.5f * (y + x / y);
	return y;
}

/* Scalar log2 approximation */
static inline float fast_log2f(float x)
{
	union { float f; u32 i; } u = { .f = x };
	int exp = (int)((u.i >> 23) & 0xFFu) - 127;
	float e = (float)exp;
	float m, p;

	u.i = (u.i & 0x7FFFFFu) | (127u << 23);
	m = u.f - 1.0f;

	p = m * 0.4808f;
	p = 0.7213f - p;
	p = m * p;
	p = 1.4425f - p;
	p = m * p;

	return e + p;
}

/* Scalar 2^x approximation */
static inline float fast_exp2f(float x)
{
	union { u32 i; float f; } v;
	int xi;
	float f;

	if (x > 60.0f)
		x = 60.0f;
	else if (x < -60.0f)
		x = -60.0f;

	xi = (int)x;
	if (x < (float)xi)
		xi--;			/* floor toward negative infinity */
	f = x - (float)xi;

	v.i = (u32)((xi + 127) << 23);	/* 2^xi */
	return v.f * (1.0f + f * (0.6931472f +
			f * (0.2402265f + f * 0.0555041f)));
}

/* Logistic sigmoid */
static inline float nap_sigmoidf(float x)
{
	return 1.0f / (1.0f + fast_exp2f(-1.4426950f * x));
}

#define NAP_FLOOR_WIN  256
#define NAP_PRIOR_K    16

/* ================================================================
 * Deterministic PRNG for weight initialization (LCG)
 * ================================================================ */

static inline float nap_prng_float(u32 *state)
{
	*state = *state * 1664525u + 1013904223u;
	return (float)(s32)*state * (1.0f / 2147483648.0f);
}

/* ================================================================
 * Weight initialization
 * ================================================================ */

#define NAP_PRNG_SEED 42u

static void nap_init_weights(struct nap_weights *w)
{
	u32 rng = NAP_PRNG_SEED;
	float scale_h1, scale_out;
	int i, j;

	/* Xavier uniform */
	scale_h1  = nap_sqrtf(6.0f / (float)(NAP_INPUT_SIZE + NAP_HIDDEN_SIZE));
	scale_out = 0.01f;

	/* Hidden layer weights */
	for (i = 0; i < NAP_INPUT_SIZE; i++)
		for (j = 0; j < NAP_HIDDEN_SIZE; j++)
			w->w_h1[i][j] = nap_prng_float(&rng) * scale_h1;

	/* Hidden biases */
	memset(w->b_h1, 0, sizeof(w->b_h1));

	/* Output weights */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++)
		w->w_out[j] = nap_prng_float(&rng) * scale_out;

	/* Output bias */
	w->b_out = 0.0f;

	/* Neuron 0: pass-through for feature[0] = log2(sleep_length) */
	for (i = 0; i < NAP_INPUT_SIZE; i++)
		w->w_h1[i][0] = 0.0f;
	w->w_h1[0][0] = 1.0f;
	w->b_h1[0] = 0.0f;
	w->w_out[0] = 1.0f;
}

static void nap_init_log2_tres(struct nap_cpu_data *d,
			       struct cpuidle_driver *drv)
{
	int i;

	for (i = 0; i < drv->state_count; i++) {
		float tres = float_max(
			(float)drv->states[i].target_residency * NSEC_PER_USEC, 1.0f);

		d->log2_tres[i] = fast_log2f(tres);
	}

	for (i = 1; i < drv->state_count; i++)
		d->weights.thr_ord[i - 1] = d->log2_tres[i];
}

/* ================================================================
 * Feature extraction helpers
 * ================================================================ */

struct logring_stats {
	float avg;
	float min;
	float max;
};

static void logring_compute(const struct nap_cpu_data *d,
			    struct logring_stats *s)
{
	int i, n = d->hist_count;
	float sum;

	if (n == 0) {
		*s = (struct logring_stats){ 0 };
		return;
	}

	sum = d->log_history[0];
	s->min = sum;
	s->max = sum;

	for (i = 1; i < n; i++) {
		float val = d->log_history[i];
		sum += val;
		s->min = float_min(s->min, val);
		s->max = float_max(s->max, val);
	}

	s->avg = sum / (float)n;
}

static void nap_extract_features(struct cpuidle_driver *drv,
				 struct cpuidle_device *dev,
				 float out[NAP_INPUT_SIZE],
				 s64 latency_req_ns)
{
	struct nap_cpu_data *d = this_cpu_ptr(&nap_data);
	struct logring_stats lr;
	ktime_t sleep_length;
	u64 busy_ns;
	float err_f;
	float abs_err;
	float log_err;
	u64 deepest_lat;
	bool lat_valid;

	sleep_length = tick_nohz_get_sleep_length();
	busy_ns = local_clock() - d->prev_idle_exit;

	err_f = (float)(d->last_prediction_error / 1000);
	abs_err = (err_f >= 0.0f) ? err_f : -err_f;

	out[0] = fast_log2f(float_max((float)ktime_to_ns(sleep_length), 1.0f));
	out[1] = fast_log2f(float_max((float)dev->last_residency * NSEC_PER_USEC, 1.0f));
	out[6] = fast_log2f(float_max((float)busy_ns, 1.0f));

	log_err = fast_log2f(abs_err + 1.0f);
	out[5] = err_f >= 0.0f ? log_err : -log_err;

	/* Update log_history ring buffer */
	d->log_history[d->hist_idx] = out[1];

	/* Compute log_history statistics */
	logring_compute(d, &lr);
	out[2] = lr.avg;
	out[3] = lr.min;
	out[4] = lr.max;

	/* out[7]: log2(latency_req) - log2(deepest_lat) */
	deepest_lat = (u64)drv->states[drv->state_count - 1].exit_latency * NSEC_PER_USEC;
	lat_valid = (latency_req_ns < PM_QOS_LATENCY_ANY_NS && deepest_lat > 0);

	if (lat_valid)
		out[7] = fast_log2f(float_max((float)latency_req_ns, 1.0f))
		       - fast_log2f(float_max((float)deepest_lat, 1.0f));
	else
		out[7] = 0.0f;

	d->last_predicted_ns = ktime_to_ns(sleep_length);
}

/* ================================================================
 * FPU entry point for nap_select
 * ================================================================ */

int nap_fpu_select(struct cpuidle_driver *drv,
		   struct cpuidle_device *dev,
		   struct nap_cpu_data *d)
{
	s64 latency_req_ns = (s64)pm_qos_request(PM_QOS_CPU_DMA_LATENCY) * NSEC_PER_USEC;

	/* Handle deferred weight reset */
	if (unlikely(d->reset_pending)) {
		nap_init_weights(&d->weights);
		nap_init_log2_tres(d, drv);
		memset(d->bin_count, 0, sizeof(d->bin_count));
		d->have_sample = false;
		d->stats.learn_count = 0;
		d->needs_learn = false;
		d->reset_pending = false;
	}

	if (d->have_sample) {
		float decay = (float)(NAP_FLOOR_WIN - 1) / (float)NAP_FLOOR_WIN;
		int k, label_bin = 0;

		if (d->needs_learn) {
			float base_lr = (float)d->learning_rate_millths / 1000.0f;
			float clamp_val = (float)d->max_grad_norm_millths / 1000.0f;
			float s = d->nn_output;
			float g = 0.0f;

			for (k = 1; k < drv->state_count; k++) {
				float th = d->active_w->thr_ord[k - 1];
				float q = nap_sigmoidf(s - th);
				u64 target_residency_ns = (u64)drv->states[k].target_residency * NSEC_PER_USEC;
				float y = (d->learn_actual_ns >= target_residency_ns) ? 1.0f : 0.0f;
				float err = q - y;
				float lo = d->log2_tres[k] - 6.0f;
				float hi = d->log2_tres[k] + 6.0f;

				g += err;
				d->active_w->thr_ord[k - 1] =
					fclampf(th + fclampf(base_lr * err,
							     -clamp_val, clamp_val),
						lo, hi);
			}
			d->learn_d_out = g;
			d->learn_lr = base_lr;
			d->stats.learn_count++;
			nap_nn_learn_c(d);
			d->needs_learn = false;
		}

		/* Floor histogram update, every idle */
		for (k = 1; k < drv->state_count; k++) {
			u64 target_residency_ns = (u64)drv->states[k].target_residency * NSEC_PER_USEC;
			if (d->learn_actual_ns >= target_residency_ns)
				label_bin = k;
		}
		for (k = 0; k < drv->state_count; k++)
			d->bin_count[k] *= decay;
		d->bin_count[label_bin] += 1.0f;

		d->have_sample = false;
	}

	/* Feature extraction + NN forward pass */
	nap_extract_features(drv, dev, d->features_f32, latency_req_ns);

	d->active_w = &d->weights;

	nap_nn_forward_c(d->features_f32, &d->nn_output, d->hidden_out,
		       d->active_w);

	/* Decision layer */
	{
		float conf = (float)d->conf_millths / 1000.0f;
		float s = d->nn_output;
		float sleep_log2 = d->features_f32[0];
		float suffix[CPUIDLE_STATE_MAX];
		float total = 0.0f;
		float qmin = 1.0f;
		int k, m = 0, idx = 0;

		for (k = 0; k < drv->state_count; k++)
			total += d->bin_count[k];

		suffix[drv->state_count - 1] =
			d->bin_count[drv->state_count - 1];
		for (k = drv->state_count - 2; k >= 0; k--)
			suffix[k] = suffix[k + 1] + d->bin_count[k];

		for (k = 1; k < drv->state_count; k++) {
			float q_nn = nap_sigmoidf(s - d->active_w->thr_ord[k - 1]);
			float q = ((float)NAP_PRIOR_K * q_nn + suffix[k]) /
				  ((float)NAP_PRIOR_K + total);

			if (d->log2_tres[k] > sleep_log2)
				q = 0.0f;	/* cannot idle past the next timer */
			if (q < qmin)
				qmin = q;
			q = qmin;

			if (q >= conf)
				m = k;
			else
				break;
		}

		for (k = m; k >= 1; k--) {
			u64 exit_latency_ns = (u64)drv->states[k].exit_latency * NSEC_PER_USEC;

			if (dev->states_usage[k].disable)
				continue;
			if (exit_latency_ns > latency_req_ns)
				continue;
			idx = k;
			break;
		}
		return idx;
	}
}

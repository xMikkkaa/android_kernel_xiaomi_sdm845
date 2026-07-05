/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NAP_H
#define NAP_H

#include <linux/cpuidle.h>
#include <linux/ktime.h>

/* ================================================================
 * Neural network dimensions
 * ================================================================ */

#define NAP_INPUT_SIZE    8
#define NAP_HIDDEN_SIZE   8
#define NAP_NUM_CUTS      (CPUIDLE_STATE_MAX - 1)

/*
 * Neural network weights for an 8-input MLP with an ordinal survival head.
 *
 * The trunk maps input[8] → hidden[8] (ReLU), feeding a shared linear score
 *   s = w_out . hidden + b_out
 * which is the input to a proportional-odds ordinal head. For each idle-state
 * boundary k the predicted survival probability that the upcoming idle reaches
 * that state's target_residency is
 *   q_k = sigmoid(s - thr_ord[k-1]).
 */
struct nap_weights {
	/* Hidden layer: input[8] → hidden[8] */
	float w_h1[NAP_INPUT_SIZE][NAP_HIDDEN_SIZE];  /* 64 params */
	float b_h1[NAP_HIDDEN_SIZE];                   /* 8 params  */
	/* Shared score head: hidden[8] → scalar s */
	float w_out[NAP_HIDDEN_SIZE];                  /* 8 params  */
	float b_out;                                   /* 1 param   */
	/* Ordinal survival head: one ordered threshold per state boundary */
	float thr_ord[NAP_NUM_CUTS];
} __aligned(32);

struct nap_cpu_data;

/* Plain C implementations (auto-vectorized on ARM64) */
void nap_nn_forward_c(const float *input, float *output,
		      float *hidden_save, const struct nap_weights *w);
void nap_nn_learn_c(struct nap_cpu_data *d);

/* ================================================================
 * Feature extraction
 * ================================================================ */

#define NAP_HISTORY_SIZE     8

/* Refresh interval for the cached minimum-valid-state lookup.  HZ
 * jiffies (1 s) bounds staleness from sysfs/runtime state-disable
 * events; PM QoS latency changes are detected immediately via the
 * cached latency_req comparison.
 */
#define NAP_MIN_STATE_REFRESH_JIFFIES  HZ

struct nap_stats {
	u64 total_selects;
	u64 total_residency_ns;
	u64 overshoot_count;
	u64 learn_count;
};

struct nap_cpu_data {
	/* Ring buffer */
	u64   history[NAP_HISTORY_SIZE];
	float log_history[NAP_HISTORY_SIZE];
	int   hist_idx;
	int   hist_count;

	/* External signal tracking */
	u64     prev_idle_exit;
	s64     last_predicted_ns;
	s64     last_prediction_error;

	/* short-circuit status */
	bool short_circuited;			/* set in select, read in reflect */
	int  cached_min_state;			/* cached shallowest valid state */
	s64  cached_min_state_latency;		/* latency_req when cache populated */
	unsigned long cached_min_state_jiffies;	/* jiffies when cache populated */

	/* Jiffies-based learning rate floor */
	unsigned long last_learn_jiffies;
	unsigned int  learn_jiffies_min;	/* 0 = disabled */

	/* select/reflect handoff */
	int   last_selected_idx;

	/* Shared ordinal score s */
	float nn_output;

	/* Align arrays to 32 bytes for optimized vector operations */
	float hidden_out[NAP_HIDDEN_SIZE] __aligned(32);
	float features_f32[NAP_INPUT_SIZE] __aligned(32);

	/* Backprop scratch */
	float learn_d_out;	/* score gradient g = sum_k (q_k - y_k) */
	float learn_lr;		/* effective learning rate (symmetric) */
	float learn_d_hid[NAP_HIDDEN_SIZE] __aligned(32);

	/* Precomputed per-state log2 thresholds */
	float log2_tres[CPUIDLE_STATE_MAX];

	/* Decayed per-bin idle histogram: robustness-floor survival estimate */
	float bin_count[CPUIDLE_STATE_MAX];

	/* Deferred learning data */
	bool  needs_learn;
	bool  have_sample;	/* a fresh residency awaits per-idle processing */
	u64   learn_actual_ns;

	/* Single network: 8→8 trunk + ordinal survival head */
	struct nap_weights weights;
	struct nap_weights *active_w;	/* always &weights */

	/* Online learning */
	unsigned int learning_rate_millths;
	unsigned int max_grad_norm_millths;
	unsigned int conf_millths;	/* decision confidence level (500 = 0.5) */
	int   learn_interval;
	int   learn_counter;
	bool reset_pending;		/* set by sysfs, consumed by nap_select */

	/* sysfs statistics */
	struct nap_stats stats;
};

DECLARE_PER_CPU(struct nap_cpu_data, nap_data);

/* FPU entry point (nap_fpu.c) — call only within kernel_neon_begin/end */
int nap_fpu_select(struct cpuidle_driver *drv,
		   struct cpuidle_device *dev,
		   struct nap_cpu_data *d);

/* sysfs interface */
int  nap_sysfs_init(void);
void nap_sysfs_exit(void);

#endif /* NAP_H */

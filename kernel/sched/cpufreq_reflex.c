// SPDX-License-Identifier: GPL-2.0
/*
 * Reflex CPUFreq governor (based on schedutil for Beryllium Linux 4.9)
 * Copyright (C) 2026 Masahito Suzuki / Adapted for 4.9 by xMikkkaa
 *
 * schedutil + idle-time accounting based hispeed floor with PELT-
 * complementary exponential decay.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <trace/events/power.h>
#include <linux/sched/sysctl.h>
#include <linux/tick.h>
#include "sched.h"
#include "tune.h"

#define CPUFREQ_REFLEX_PROGNAME "Reflex CPUFreq Governor"
#define CPUFREQ_REFLEX_AUTHOR   "Masahito Suzuki"
#define CPUFREQ_REFLEX_VERSION  "0.3.1-4.9"

#define CPUFREQ_REFLEX_DEFAULT_HISPEED_WINDOW_US   4000
#define CPUFREQ_REFLEX_DEFAULT_HISPEED_FILTER_SHIFT   1

#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)

struct rfx_tunables {
	struct gov_attr_set attr_set;
	unsigned int rate_limit_us;
	unsigned int hispeed_window_us;    /* observation window (usec) */
	unsigned int hispeed_filter_shift; /* EWMA down-ramp shift (0=off) */
	bool pl;
};

struct rfx_policy {
	struct cpufreq_policy *policy;

	struct rfx_tunables *tunables;
	struct list_head tunables_hook;

	raw_spinlock_t update_lock;  /* For shared policies */
	u64 last_freq_update_time;
	s64 freq_update_delay_ns;
	u64 last_ws;
	u64 curr_cycles;
	u64 last_cyc_update_time;
	unsigned long avg_cap;
	unsigned int next_freq;
	unsigned int cached_raw_freq;
	unsigned long hispeed_util;
	unsigned long max;

	/* The next fields are only needed if fast switch cannot be used. */
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;

	bool need_freq_update;
};

struct rfx_cpu {
	struct update_util_data update_util;
	struct rfx_policy *rfx_policy;

	bool iowait_boost_pending;
	unsigned int iowait_boost;
	unsigned int iowait_boost_max;
	u64 last_update;

	struct sched_walt_cpu_load walt_load;

	/* The fields below are only needed when sharing a policy. */
	unsigned long util;
	unsigned long max;
	unsigned int flags;
	unsigned int cpu;

	/* Idle-time accounting for hispeed decisions */
	u64 prev_idle_time;
	u64 prev_wall_time;
	unsigned int busy_pct;
	unsigned int filtered_busy_pct;
	bool hispeed_active;
	u64 hispeed_start_ns;
	s32 log_hispeed;	  /* hispeed_util in log32fpmax_corr */
	unsigned int hispeed_idle_windows;

	/* The field below is for single-CPU policies only. */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct rfx_cpu, rfx_cpu);
static DEFINE_PER_CPU(struct rfx_tunables *, rfx_cached_tunables);
static struct rfx_tunables *rfx_global_tunables;
static DEFINE_MUTEX(rfx_global_tunables_lock);
static unsigned int rfx_stale_ns;

/**************************************************************
 * Minimal intfp log-domain helpers for hispeed decay.
 * Specialized from intfp.h (v1.4) for u32 <-> log32fpmax_corr.
 */
#define RFX_LOG_OFP		26
#define RFX_LOG_0		S32_MIN
#define RFX_LOG_DECAY_PER_MS	(-2097152)	/* log2(0.97857206) in Q5.26 */
#define RFX_LOG_DECAY_MAX_MS	320		/* 10 half-lives */

static const u16 rfx_enc_corr_lut[256] = {
	    0,    89,   177,   264,   350,   436,   521,   606,   690,   773,   855,   937,  1018,  1098,  1178,  1257,
	 1335,  1413,  1489,  1565,  1641,  1716,  1790,  1863,  1936,  2008,  2079,  2150,  2219,  2289,  2357,  2425,
	 2492,  2558,  2624,  2689,  2753,  2817,  2880,  2942,  3004,  3065,  3125,  3184,  3243,  3301,  3358,  3415,
	 3471,  3526,  3581,  3635,  3688,  3740,  3792,  3843,  3894,  3943,  3992,  4041,  4088,  4135,  4182,  4227,
	 4272,  4316,  4360,  4402,  4444,  4486,  4526,  4566,  4606,  4644,  4682,  4719,  4756,  4792,  4827,  4861,
	 4895,  4928,  4960,  4992,  5023,  5053,  5083,  5112,  5140,  5167,  5194,  5220,  5245,  5270,  5294,  5317,
	 5340,  5362,  5383,  5404,  5423,  5443,  5461,  5479,  5496,  5512,  5528,  5543,  5557,  5570,  5583,  5596,
	 5607,  5618,  5628,  5637,  5646,  5654,  5661,  5668,  5674,  5679,  5683,  5687,  5690,  5693,  5695,  5696,
	 5696,  5696,  5695,  5693,  5690,  5687,  5683,  5679,  5674,  5668,  5661,  5654,  5646,  5637,  5628,  5618,
	 5607,  5596,  5583,  5570,  5557,  5543,  5528,  5512,  5496,  5479,  5461,  5443,  5423,  5404,  5383,  5362,
	 5340,  5317,  5294,  5270,  5245,  5220,  5194,  5167,  5140,  5112,  5083,  5053,  5023,  4992,  4960,  4928,
	 4895,  4861,  4827,  4792,  4756,  4719,  4682,  4644,  4606,  4566,  4526,  4486,  4444,  4402,  4360,  4316,
	 4272,  4227,  4182,  4135,  4088,  4041,  3992,  3943,  3894,  3843,  3792,  3740,  3688,  3635,  3581,  3526,
	 3471,  3415,  3358,  3301,  3243,  3184,  3125,  3065,  3004,  2942,  2880,  2817,  2753,  2689,  2624,  2558,
	 2492,  2425,  2357,  2289,  2219,  2150,  2079,  2008,  1936,  1863,  1790,  1716,  1641,  1565,  1489,  1413,
	 1335,  1257,  1178,  1098,  1018,   937,   855,   773,   690,   606,   521,   436,   350,   264,   177,    89,
};

static const u16 rfx_dec_corr_lut[256] = {
	    0,    88,   175,   261,   346,   431,   516,   599,   682,   764,   846,   926,  1006,  1086,  1165,  1243,
	 1320,  1397,  1473,  1548,  1622,  1696,  1770,  1842,  1914,  1985,  2056,  2125,  2194,  2263,  2331,  2398,
	 2464,  2530,  2595,  2659,  2722,  2785,  2848,  2909,  2970,  3030,  3090,  3148,  3206,  3264,  3321,  3377,
	 3432,  3487,  3541,  3594,  3646,  3698,  3750,  3800,  3850,  3899,  3948,  3995,  4042,  4089,  4135,  4180,
	 4224,  4268,  4311,  4353,  4394,  4435,  4476,  4515,  4554,  4592,  4630,  4666,  4702,  4738,  4773,  4807,
	 4840,  4873,  4905,  4936,  4966,  4996,  5026,  5054,  5082,  5109,  5136,  5161,  5186,  5211,  5235,  5258,
	 5280,  5302,  5323,  5343,  5362,  5381,  5400,  5417,  5434,  5450,  5466,  5480,  5494,  5508,  5521,  5533,
	 5544,  5555,  5565,  5574,  5582,  5590,  5598,  5604,  5610,  5615,  5620,  5623,  5626,  5629,  5631,  5632,
	 5632,  5632,  5631,  5629,  5626,  5623,  5620,  5615,  5610,  5604,  5598,  5590,  5582,  5574,  5565,  5555,
	 5544,  5533,  5521,  5508,  5494,  5480,  5466,  5450,  5434,  5417,  5400,  5381,  5362,  5343,  5323,  5302,
	 5280,  5258,  5235,  5211,  5186,  5161,  5136,  5109,  5082,  5054,  5026,  4996,  4966,  4936,  4905,  4873,
	 4840,  4807,  4773,  4738,  4702,  4666,  4630,  4592,  4554,  4515,  4476,  4435,  4394,  4353,  4311,  4268,
	 4224,  4180,  4135,  4089,  4042,  3995,  3948,  3899,  3850,  3800,  3750,  3698,  3646,  3594,  3541,  3487,
	 3432,  3377,  3321,  3264,  3206,  3148,  3090,  3030,  2970,  2909,  2848,  2785,  2722,  2659,  2595,  2530,
	 2464,  2398,  2331,  2263,  2194,  2125,  2056,  1985,  1914,  1842,  1770,  1696,  1622,  1548,  1473,  1397,
	 1320,  1243,  1165,  1086,  1006,   926,   846,   764,   682,   599,   516,   431,   346,   261,   175,    88,
};

static inline s32 rfx_lin_to_log(u32 v)
{
	u8 clz;
	u32 m, mf;
	u8 idx;

	if (!v)
		return RFX_LOG_0;

	clz = __builtin_clz(v);
	m = (v << clz) >> (32 - 1 - RFX_LOG_OFP);
	mf = m & ((1U << RFX_LOG_OFP) - 1);
	idx = (u8)(mf >> (RFX_LOG_OFP - 8));
	m += (u32)rfx_enc_corr_lut[idx] << (RFX_LOG_OFP - 16);

	return (s32)(((u32)(30 - clz) << RFX_LOG_OFP) + m);
}

static inline u32 rfx_log_to_lin(s32 v)
{
	bool negative;
	s32 e;
	u32 m, norm, mh;
	u8 idx;

	if (v == RFX_LOG_0)
		return 0;

	negative = v < 0;
	if (negative)
		v = -v;
	e = v >> RFX_LOG_OFP;
	if (negative)
		e = -e;

	if (e < 0)
		return 0;
	if (e >= 32)
		return U32_MAX;

	m = v & ((1U << RFX_LOG_OFP) - 1);
	norm = (1U << 31) | (m << (31 - RFX_LOG_OFP));
	mh = m << (31 - RFX_LOG_OFP);
	idx = (u8)(mh >> (31 - 8));
	norm -= (u32)rfx_dec_corr_lut[idx] << (31 - 16);

	return norm >> (31 - e);
}

/************************ Governor internals ***********************/

static bool rfx_should_update_freq(struct rfx_policy *rfx_pol, u64 time)
{
	s64 delta_ns;

	if (unlikely(rfx_pol->need_freq_update))
		return true;

	delta_ns = time - rfx_pol->last_freq_update_time;
	return delta_ns >= rfx_pol->freq_update_delay_ns;
}

static void rfx_update_commit(struct rfx_policy *rfx_pol, u64 time,
			      unsigned int next_freq)
{
	struct cpufreq_policy *policy = rfx_pol->policy;

	if (rfx_pol->next_freq == next_freq)
		return;

	rfx_pol->next_freq = next_freq;
	rfx_pol->last_freq_update_time = time;

	if (policy->fast_switch_enabled) {
		next_freq = cpufreq_driver_fast_switch(policy, next_freq);
		if (!next_freq)
			return;

		policy->cur = next_freq;
	} else {
		rfx_pol->work_in_progress = true;
		sched_irq_work_queue(&rfx_pol->irq_work);
	}
}

static unsigned int get_next_freq(struct rfx_policy *rfx_pol,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;

	freq = map_util_freq(util, freq, max);

	if (freq == rfx_pol->cached_raw_freq && !rfx_pol->need_freq_update)
		return rfx_pol->next_freq;

	rfx_pol->need_freq_update = false;
	rfx_pol->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static void rfx_get_util(unsigned long *util, unsigned long *max, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long cfs_max;
	struct rfx_cpu *loadcpu = &per_cpu(rfx_cpu, cpu);

	cfs_max = arch_scale_cpu_capacity(NULL, cpu);

	*util = min(rq->cfs.avg.util_avg, cfs_max);
	*max = cfs_max;

	*util = boosted_cpu_util(cpu, &loadcpu->walt_load);

#ifdef CONFIG_UCLAMP_TASK
	*util = uclamp_util_with(rq, *util, NULL);
	*util = min(*max, *util);
#endif
}

/************************ Hispeed (idle-time accounting) ***********************/

static void rfx_update_busy_pct(struct rfx_cpu *rfx_c,
				unsigned int window_us,
				unsigned int filter_shift, u64 time,
				unsigned long max_cap)
{
	u64 cur_idle, cur_wall;
	unsigned int wall_delta, idle_delta;

	cur_idle = get_cpu_idle_time(rfx_c->cpu, &cur_wall, 1);
	wall_delta = (unsigned int)(cur_wall - rfx_c->prev_wall_time);

	if (wall_delta >= window_us) {
		rfx_c->busy_pct = 0;
		rfx_c->hispeed_active = true;
		rfx_c->prev_idle_time = cur_idle;
		rfx_c->prev_wall_time = cur_wall;
		return;
	}

	if (!rfx_c->hispeed_active)
		return;

	rfx_c->hispeed_active = false;

	if (cur_idle > rfx_c->prev_idle_time)
		idle_delta = (unsigned int)(cur_idle - rfx_c->prev_idle_time);
	else
		idle_delta = 0;

	if (wall_delta > idle_delta)
		rfx_c->busy_pct = 100 * (wall_delta - idle_delta) / wall_delta;
	else
		rfx_c->busy_pct = 0;

	rfx_c->prev_idle_time = cur_idle;
	rfx_c->prev_wall_time = cur_wall;

	if (!filter_shift || rfx_c->busy_pct >= rfx_c->filtered_busy_pct) {
		rfx_c->filtered_busy_pct = rfx_c->busy_pct;
	} else {
		unsigned int step =
			(rfx_c->filtered_busy_pct - rfx_c->busy_pct)
			>> filter_shift;
		if (step)
			rfx_c->filtered_busy_pct -= step;
		else
			rfx_c->filtered_busy_pct = rfx_c->busy_pct;
	}

	if (rfx_c->filtered_busy_pct > 0) {
		rfx_c->hispeed_idle_windows = 0;
		if (!rfx_c->hispeed_start_ns)
			rfx_c->hispeed_start_ns = time;
		rfx_c->log_hispeed = rfx_lin_to_log(
			max_cap * rfx_c->filtered_busy_pct / 100);
	} else {
		rfx_c->hispeed_idle_windows++;
		if (rfx_c->hispeed_idle_windows >= 2) {
			rfx_c->hispeed_start_ns = 0;
			rfx_c->filtered_busy_pct = 0;
			rfx_c->log_hispeed = RFX_LOG_0;
		}
	}
}

static unsigned long rfx_blend_util(struct rfx_cpu *rfx_c,
				    unsigned long pelt_util,
				    unsigned long max_cap,
				    u64 time)
{
	unsigned long hispeed_util, hispeed_decayed;
	unsigned int elapsed_ms;
	s32 log_decayed;

	if (!rfx_c->filtered_busy_pct || !rfx_c->hispeed_start_ns)
		return pelt_util;

	hispeed_util = max_cap * rfx_c->filtered_busy_pct / 100;

	if (hispeed_util <= pelt_util)
		return pelt_util;

	elapsed_ms = (unsigned int)((time - rfx_c->hispeed_start_ns)
				    / NSEC_PER_MSEC);
	if (elapsed_ms >= RFX_LOG_DECAY_MAX_MS)
		return pelt_util;

	log_decayed = rfx_c->log_hispeed +
		      (s32)elapsed_ms * RFX_LOG_DECAY_PER_MS;
	hispeed_decayed = rfx_log_to_lin(log_decayed);

	if (hispeed_decayed <= pelt_util)
		return pelt_util;

	return min(pelt_util + hispeed_decayed, hispeed_util);
}

/************************ I/O wait boost & WALT ***********************/

static void rfx_iowait_boost(struct rfx_cpu *rfx_c, unsigned long *util,
			     unsigned long *max)
{
	unsigned int boost_util, boost_max;

	if (!rfx_c->iowait_boost)
		return;

	if (rfx_c->iowait_boost_pending) {
		rfx_c->iowait_boost_pending = false;
	} else {
		rfx_c->iowait_boost >>= 1;
		if (rfx_c->iowait_boost < rfx_c->rfx_policy->policy->min) {
			rfx_c->iowait_boost = 0;
			return;
		}
	}

	boost_util = rfx_c->iowait_boost;
	boost_max = rfx_c->iowait_boost_max;

	if (*util * boost_max < *max * boost_util) {
		*util = boost_util;
		*max = boost_max;
	}
}

static void rfx_set_iowait_boost(struct rfx_cpu *rfx_c, u64 time,
				 unsigned int flags)
{
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;

	if (rfx_c->iowait_boost) {
		s64 delta_ns = time - rfx_c->last_update;
		if (delta_ns > TICK_NSEC) {
			rfx_c->iowait_boost = 0;
			rfx_c->iowait_boost_pending = false;
		}
	}

	if (flags & SCHED_CPUFREQ_IOWAIT) {
		if (rfx_c->iowait_boost_pending)
			return;

		if (rfx_c->iowait_boost) {
			rfx_c->iowait_boost <<= 1;
			if (rfx_c->iowait_boost > rfx_c->iowait_boost_max)
				rfx_c->iowait_boost = rfx_c->iowait_boost_max;
		} else {
			rfx_c->iowait_boost = rfx_pol->policy->min;
		}
		rfx_c->iowait_boost_pending = true;
	}
}

#define NL_RATIO 75
static void rfx_walt_adjust(struct rfx_cpu *rfx_c, unsigned long *util,
			    unsigned long *max)
{
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	bool is_migration = rfx_c->flags & SCHED_CPUFREQ_INTERCLUSTER_MIG;
	unsigned long nl = rfx_c->walt_load.nl;
	unsigned long cpu_util = rfx_c->util;
	bool is_hiload;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	is_hiload = (cpu_util >= mult_frac(rfx_pol->avg_cap,
					   90, /* default hispeed load */
					   100));

	if (is_hiload && !is_migration)
		*util = max(*util, rfx_pol->hispeed_util);

	if (is_hiload && nl >= mult_frac(cpu_util, NL_RATIO, 100))
		*util = *max;

	if (rfx_pol->tunables->pl)
		*util = max(*util, rfx_c->walt_load.pl);
}

#define KHZ 1000
static void rfx_track_cycles(struct rfx_policy *rfx_pol,
			     unsigned int prev_freq,
			     u64 upto)
{
	u64 delta_ns, cycles;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	delta_ns = upto - rfx_pol->last_cyc_update_time;
	delta_ns *= prev_freq;
	do_div(delta_ns, (NSEC_PER_SEC / KHZ));
	cycles = delta_ns;
	rfx_pol->curr_cycles += cycles;
	rfx_pol->last_cyc_update_time = upto;
}

static void rfx_calc_avg_cap(struct rfx_policy *rfx_pol, u64 curr_ws,
			     unsigned int prev_freq)
{
	u64 last_ws = rfx_pol->last_ws;
	unsigned int avg_freq;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	if (curr_ws == last_ws) {
		rfx_track_cycles(rfx_pol, prev_freq, sched_ktime_clock());
		return;
	}

	if (curr_ws > last_ws + sched_ravg_window) {
		avg_freq = prev_freq;
		rfx_pol->last_cyc_update_time = curr_ws;
	} else {
		rfx_track_cycles(rfx_pol, prev_freq, curr_ws);
		avg_freq = rfx_pol->curr_cycles;
		avg_freq /= sched_ravg_window / (NSEC_PER_SEC / KHZ);
	}

	// Simple frequency to util mapping
	rfx_pol->avg_cap = mult_frac(rfx_pol->max, avg_freq,
				     rfx_pol->policy->cpuinfo.max_freq);
	rfx_pol->curr_cycles = 0;
	rfx_pol->last_ws = curr_ws;
}

#ifdef CONFIG_NO_HZ_COMMON
static bool rfx_cpu_is_busy(struct rfx_cpu *rfx_c)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls();
	bool ret = idle_calls == rfx_c->saved_idle_calls;

	rfx_c->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool rfx_cpu_is_busy(struct rfx_cpu *rfx_c) { return false; }
#endif

/************************ Core update callbacks ***********************/

static void rfx_update_single(struct update_util_data *hook, u64 time,
			      unsigned int flags)
{
	struct rfx_cpu *rfx_c = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	unsigned long util, max;
	unsigned int next_f;
	bool busy;

	flags &= ~SCHED_CPUFREQ_RT_DL;

	if (!rfx_pol->tunables->pl && flags & SCHED_CPUFREQ_PL)
		return;

	rfx_set_iowait_boost(rfx_c, time, flags);
	rfx_c->last_update = time;

	if (!rfx_should_update_freq(rfx_pol, time))
		return;

	busy = rfx_cpu_is_busy(rfx_c);

	raw_spin_lock(&rfx_pol->update_lock);
	if (flags & SCHED_CPUFREQ_RT_DL) {
		rfx_pol->cached_raw_freq = 0;
		next_f = rfx_pol->policy->cpuinfo.max_freq;
	} else {
		rfx_get_util(&util, &max, rfx_c->cpu);
		rfx_pol->max = max;

		rfx_c->util = util;
		rfx_c->max = max;
		rfx_c->flags = flags;

		rfx_calc_avg_cap(rfx_pol, rfx_c->walt_load.ws,
				 rfx_pol->policy->cur);

		rfx_iowait_boost(rfx_c, &util, &max);
		rfx_walt_adjust(rfx_c, &util, &max);

		/* Blend WALT/PELT util with Reflex busy% decay */
		rfx_update_busy_pct(rfx_c, tunables->hispeed_window_us,
				    tunables->hispeed_filter_shift, time, max);
		util = rfx_blend_util(rfx_c, util, max, time);

		next_f = get_next_freq(rfx_pol, util, max);

		if (busy && next_f < rfx_pol->next_freq) {
			next_f = rfx_pol->next_freq;
			rfx_pol->cached_raw_freq = 0;
		}
	}
	rfx_update_commit(rfx_pol, time, next_f);
	raw_spin_unlock(&rfx_pol->update_lock);
}

static unsigned int rfx_next_freq_shared(struct rfx_cpu *rfx_c, u64 time)
{
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;

	for_each_cpu(j, policy->cpus) {
		struct rfx_cpu *j_rfx_c = &per_cpu(rfx_cpu, j);
		unsigned long j_util, j_max;
		s64 delta_ns;

		delta_ns = time - j_rfx_c->last_update;
		if (delta_ns > rfx_stale_ns) {
			j_rfx_c->iowait_boost = 0;
			j_rfx_c->iowait_boost_pending = false;
			continue;
		}

		if (j_rfx_c->flags & SCHED_CPUFREQ_RT_DL) {
			rfx_pol->cached_raw_freq = 0;
			return policy->cpuinfo.max_freq;
		}

		j_util = j_rfx_c->util;
		j_max = j_rfx_c->max;
		if (j_util * max >= j_max * util) {
			util = j_util;
			max = j_max;
		}

		rfx_iowait_boost(j_rfx_c, &util, &max);
		rfx_walt_adjust(j_rfx_c, &util, &max);

		/* Blend WALT/PELT util with Reflex busy% decay per CPU */
		rfx_update_busy_pct(j_rfx_c, tunables->hispeed_window_us,
				    tunables->hispeed_filter_shift, time, max);
		util = rfx_blend_util(j_rfx_c, util, max, time);
	}

	return get_next_freq(rfx_pol, util, max);
}

static void rfx_update_shared(struct update_util_data *hook, u64 time,
			      unsigned int flags)
{
	struct rfx_cpu *rfx_c = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	unsigned long util, max;
	unsigned int next_f;

	if (!rfx_pol->tunables->pl && flags & SCHED_CPUFREQ_PL)
		return;

	rfx_get_util(&util, &max, rfx_c->cpu);

	flags &= ~SCHED_CPUFREQ_RT_DL;

	raw_spin_lock(&rfx_pol->update_lock);

	rfx_pol->max = max;

	rfx_c->util = util;
	rfx_c->max = max;
	rfx_c->flags = flags;

	rfx_set_iowait_boost(rfx_c, time, flags);
	rfx_c->last_update = time;

	rfx_calc_avg_cap(rfx_pol, rfx_c->walt_load.ws,
			 rfx_pol->policy->cur);

	if (rfx_should_update_freq(rfx_pol, time)) {
		if (flags & SCHED_CPUFREQ_RT_DL) {
			next_f = rfx_pol->policy->cpuinfo.max_freq;
			rfx_pol->cached_raw_freq = 0;
		} else {
			next_f = rfx_next_freq_shared(rfx_c, time);
		}
		rfx_update_commit(rfx_pol, time, next_f);
	}
	raw_spin_unlock(&rfx_pol->update_lock);
}

/************************ Kthread (slow path) ***********************/

static void rfx_work(struct kthread_work *work)
{
	struct rfx_policy *rfx_pol = container_of(work, struct rfx_policy, work);
	unsigned int freq;
	unsigned long flags;

	raw_spin_lock_irqsave(&rfx_pol->update_lock, flags);
	freq = rfx_pol->next_freq;
	rfx_pol->work_in_progress = false;
	raw_spin_unlock_irqrestore(&rfx_pol->update_lock, flags);

	mutex_lock(&rfx_pol->work_lock);
	__cpufreq_driver_target(rfx_pol->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&rfx_pol->work_lock);
}

static void rfx_irq_work(struct irq_work *irq_work)
{
	struct rfx_policy *rfx_pol;

	rfx_pol = container_of(irq_work, struct rfx_policy, irq_work);

	kthread_queue_work(&rfx_pol->worker, &rfx_pol->work);
}

/************************** sysfs interface ************************/

static inline struct rfx_tunables *to_rfx_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct rfx_tunables, attr_set);
}

static ssize_t pl_show(struct gov_attr_set *attr_set, char *buf)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->pl);
}

static ssize_t pl_store(struct gov_attr_set *attr_set, const char *buf,
			size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);

	if (kstrtobool(buf, &tunables->pl))
		return -EINVAL;
	return count;
}

#define RFX_TUNABLE_UINT(name)						\
static ssize_t name##_show(struct gov_attr_set *attr_set, char *buf)	\
{									\
	struct rfx_tunables *t = to_rfx_tunables(attr_set);		\
	return sprintf(buf, "%u\n", t->name);				\
}									\
static ssize_t								\
name##_store(struct gov_attr_set *attr_set, const char *buf, size_t count) \
{									\
	struct rfx_tunables *t = to_rfx_tunables(attr_set);		\
	unsigned int val;						\
	if (kstrtouint(buf, 10, &val))					\
		return -EINVAL;						\
	t->name = val;							\
	return count;							\
}									\
static struct governor_attr name = __ATTR_RW(name)

static ssize_t rfx_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	return sprintf(buf, "%u\n", tunables->rate_limit_us);
}

static ssize_t rfx_rate_limit_us_store(struct gov_attr_set *attr_set, const char *buf,
				       size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->rate_limit_us = rate_limit_us;

	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_pol->freq_update_delay_ns = rate_limit_us * NSEC_PER_USEC;

	return count;
}

static struct governor_attr rfx_rate_limit_us =
	__ATTR(rate_limit_us, 0644, rfx_rate_limit_us_show, rfx_rate_limit_us_store);

static ssize_t version_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%s\n", CPUFREQ_REFLEX_VERSION);
}
static struct governor_attr version = __ATTR_RO(version);

RFX_TUNABLE_UINT(hispeed_window_us);
RFX_TUNABLE_UINT(hispeed_filter_shift);
static struct governor_attr pl = __ATTR_RW(pl);

static struct attribute *rfx_attributes[] = {
	&version.attr,
	&rfx_rate_limit_us.attr,
	&hispeed_window_us.attr,
	&hispeed_filter_shift.attr,
	&pl.attr,
	NULL
};

static void rfx_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);
	kfree(to_rfx_tunables(attr_set));
}

static struct kobj_type rfx_tunables_ktype = {
	.default_attrs = rfx_attributes,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &rfx_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor reflex_gov;

static struct rfx_policy *rfx_policy_alloc(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol;

	rfx_pol = kzalloc(sizeof(*rfx_pol), GFP_KERNEL);
	if (!rfx_pol)
		return NULL;

	rfx_pol->policy = policy;
	raw_spin_lock_init(&rfx_pol->update_lock);
	return rfx_pol;
}

static void rfx_policy_free(struct rfx_policy *rfx_pol)
{
	kfree(rfx_pol);
}

static int rfx_kthread_create(struct rfx_policy *rfx_pol)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO - 1 };
	struct cpufreq_policy *policy = rfx_pol->policy;
	int ret;

	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&rfx_pol->work, rfx_work);
	kthread_init_worker(&rfx_pol->worker);
	thread = kthread_create(kthread_worker_fn, &rfx_pol->worker,
				"rfxgov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("reflex: failed to create kthread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	rfx_pol->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&rfx_pol->irq_work, rfx_irq_work);
	mutex_init(&rfx_pol->work_lock);

	wake_up_process(thread);

	return 0;
}

static void rfx_kthread_stop(struct rfx_policy *rfx_pol)
{
	if (rfx_pol->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&rfx_pol->worker);
	kthread_stop(rfx_pol->thread);
	mutex_destroy(&rfx_pol->work_lock);
}

static struct rfx_tunables *rfx_tunables_alloc(struct rfx_policy *rfx_pol)
{
	struct rfx_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &rfx_pol->tunables_hook);
		if (!have_governor_per_policy())
			rfx_global_tunables = tunables;
	}
	return tunables;
}

static void rfx_tunables_save(struct cpufreq_policy *policy,
			      struct rfx_tunables *tunables)
{
	int cpu;
	struct rfx_tunables *cached = per_cpu(rfx_cached_tunables, policy->cpu);

	if (!have_governor_per_policy())
		return;

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached) {
			pr_warn("reflex: Couldn't allocate tunables for caching\n");
			return;
		}
		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(rfx_cached_tunables, cpu) = cached;
	}

	cached->pl = tunables->pl;
	cached->hispeed_window_us = tunables->hispeed_window_us;
	cached->hispeed_filter_shift = tunables->hispeed_filter_shift;
	cached->rate_limit_us = tunables->rate_limit_us;
}

static void rfx_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		rfx_global_tunables = NULL;
}

static void rfx_tunables_restore(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	struct rfx_tunables *cached = per_cpu(rfx_cached_tunables, policy->cpu);

	if (!cached)
		return;

	tunables->pl = cached->pl;
	tunables->hispeed_window_us = cached->hispeed_window_us;
	tunables->hispeed_filter_shift = cached->hispeed_filter_shift;
	tunables->rate_limit_us = cached->rate_limit_us;
}

static int rfx_init(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol;
	struct rfx_tunables *tunables;
	int ret = 0;

	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	rfx_pol = rfx_policy_alloc(policy);
	if (!rfx_pol) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = rfx_kthread_create(rfx_pol);
	if (ret)
		goto free_rfx_pol;

	mutex_lock(&rfx_global_tunables_lock);

	if (rfx_global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = rfx_pol;
		rfx_pol->tunables = rfx_global_tunables;

		gov_attr_set_get(&rfx_global_tunables->attr_set, &rfx_pol->tunables_hook);
		goto out;
	}

	tunables = rfx_tunables_alloc(rfx_pol);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->rate_limit_us = 2000; // default rate limit
	tunables->hispeed_window_us = CPUFREQ_REFLEX_DEFAULT_HISPEED_WINDOW_US;
	tunables->hispeed_filter_shift = CPUFREQ_REFLEX_DEFAULT_HISPEED_FILTER_SHIFT;
	tunables->pl = false;

	policy->governor_data = rfx_pol;
	rfx_pol->tunables = tunables;
	rfx_stale_ns = sched_ravg_window + (sched_ravg_window >> 3);

	rfx_tunables_restore(policy);

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &rfx_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   reflex_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&rfx_global_tunables_lock);
	return 0;

fail:
	policy->governor_data = NULL;
	rfx_clear_global_tunables();

stop_kthread:
	rfx_kthread_stop(rfx_pol);
	mutex_unlock(&rfx_global_tunables_lock);

free_rfx_pol:
	rfx_policy_free(rfx_pol);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("reflex: initialization failed (error %d)\n", ret);
	return ret;
}

static void rfx_exit(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	unsigned int count;

	mutex_lock(&rfx_global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &rfx_pol->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		rfx_tunables_save(policy, tunables);
		rfx_clear_global_tunables();
	}

	mutex_unlock(&rfx_global_tunables_lock);

	rfx_kthread_stop(rfx_pol);
	rfx_policy_free(rfx_pol);
	cpufreq_disable_fast_switch(policy);
}

static int rfx_start(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	unsigned int cpu;

	rfx_pol->freq_update_delay_ns =
		rfx_pol->tunables->rate_limit_us * NSEC_PER_USEC;
	rfx_pol->last_freq_update_time = 0;
	rfx_pol->next_freq = 0;
	rfx_pol->work_in_progress = false;
	rfx_pol->need_freq_update = false;
	rfx_pol->cached_raw_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct rfx_cpu *rfx_c = &per_cpu(rfx_cpu, cpu);

		memset(rfx_c, 0, sizeof(*rfx_c));
		rfx_c->rfx_policy = rfx_pol;
		rfx_c->cpu = cpu;
		rfx_c->flags = SCHED_CPUFREQ_RT;
		rfx_c->iowait_boost_max = policy->cpuinfo.max_freq;
		rfx_c->prev_idle_time = get_cpu_idle_time(cpu,
					&rfx_c->prev_wall_time, 1);
	}

	for_each_cpu(cpu, policy->cpus) {
		struct rfx_cpu *rfx_c = &per_cpu(rfx_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &rfx_c->update_util,
					     policy_is_shared(policy) ?
							rfx_update_shared :
							rfx_update_single);
	}
	return 0;
}

static void rfx_stop(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_sched();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&rfx_pol->irq_work);
		kthread_cancel_work_sync(&rfx_pol->work);
	}
}

static void rfx_limits(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	unsigned long flags;
	unsigned int ret;
	int cpu;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&rfx_pol->work_lock);
		raw_spin_lock_irqsave(&rfx_pol->update_lock, flags);
		rfx_track_cycles(rfx_pol, rfx_pol->policy->cur,
				 sched_ktime_clock());
		raw_spin_unlock_irqrestore(&rfx_pol->update_lock, flags);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&rfx_pol->work_lock);
	} else {
		ret = cpufreq_policy_apply_limits_fast(policy);
		if (ret && policy->cur != ret) {
			policy->cur = ret;
			for_each_cpu(cpu, policy->cpus)
				trace_cpu_frequency(ret, cpu);
		}
	}

	rfx_pol->need_freq_update = true;
}

static struct cpufreq_governor reflex_gov = {
	.name = "reflex",
	.owner = THIS_MODULE,
	.init = rfx_init,
	.exit = rfx_exit,
	.start = rfx_start,
	.stop = rfx_stop,
	.limits = rfx_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_REFLEX
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &reflex_gov;
}
#endif

static int __init rfx_register(void)
{
	return cpufreq_register_governor(&reflex_gov);
}
fs_initcall(rfx_register);

MODULE_AUTHOR(CPUFREQ_REFLEX_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(CPUFREQ_REFLEX_PROGNAME);

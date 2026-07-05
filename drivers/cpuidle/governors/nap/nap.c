// SPDX-License-Identifier: GPL-2.0
/*
 * nap.c — Neural Adaptive Predictor cpuidle governor
 *
 * Adapted for Linux 4.9 on ARM64.
 *
 * IMPORTANT: This file is compiled WITHOUT FPU/SIMD flags.
 * All floating-point and SIMD code lives in nap_fpu.c and nap_nn_c.c.
 * We use kernel_neon_begin() and kernel_neon_end() to save/restore FPU state.
 */

#include <linux/cpuidle.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/math64.h>

#include <linux/string.h>
#include <linux/tick.h>
#include <linux/pm_qos.h>
#include <asm/simd.h>
#include <asm/neon.h>

#include "nap.h"

#define CPUIDLE_NAP_PROGNAME "Nap CPUIdle Governor"
#define CPUIDLE_NAP_AUTHOR   "Masahito Suzuki"
#define CPUIDLE_NAP_VERSION  "0.5.0"

/* Governor defaults */
#define NAP_DEFAULT_LR_MILLTHS    1     /* 0.001 = 1 millths */
#define NAP_DEFAULT_INTERVAL      4     /* learn every 4 reflects */
#define NAP_DEFAULT_CLAMP_MILLTHS 1000  /* 1.0 = 1000 millths */
#define NAP_DEFAULT_CONF_MILLTHS  500   /* 0.5 = balanced survival confidence */

#define RESIDENCY_THRESHOLD_NS    1500000ULL

/* ================================================================
 * Per-CPU data
 * ================================================================ */

DEFINE_PER_CPU(struct nap_cpu_data, nap_data);
static struct cpuidle_driver *nap_cached_drv;

/* ================================================================
 * Reflect-time updates (integer-only, no FPU needed)
 * ================================================================ */

static void nap_history_update(struct nap_cpu_data *d, u64 measured_ns)
{
	d->history[d->hist_idx] = measured_ns;
	d->hist_idx = (d->hist_idx + 1) % NAP_HISTORY_SIZE;
	if (d->hist_count < NAP_HISTORY_SIZE)
		d->hist_count++;
}

static void nap_update_external_signals(struct nap_cpu_data *d)
{
	d->prev_idle_exit = local_clock();
}

/* ================================================================
 * Governor callbacks
 * ================================================================ */

static int nap_fallback_heuristic(struct cpuidle_driver *drv,
				  struct cpuidle_device *dev)
{
	s64 latency_req_ns = (s64)pm_qos_request(PM_QOS_CPU_DMA_LATENCY) * NSEC_PER_USEC;
	u64 sleep_length_ns = ktime_to_ns(tick_nohz_get_sleep_length());
	int i;

	for (i = drv->state_count - 1; i > 0; i--) {
		u64 exit_latency_ns = (u64)drv->states[i].exit_latency * NSEC_PER_USEC;
		u64 target_residency_ns = (u64)drv->states[i].target_residency * NSEC_PER_USEC;

		if (dev->states_usage[i].disable)
			continue;
		if (exit_latency_ns > latency_req_ns)
			continue;
		if (target_residency_ns > sleep_length_ns)
			continue;
		return i;
	}
	return 0;
}

/*
 * Return the shallowest enabled C-state that satisfies the current
 * latency request, or 0 if none exists (POLL is the only option).
 * Does not consult the NN.
 */
static int nap_find_min_valid_state(struct cpuidle_driver *drv,
				    struct cpuidle_device *dev,
				    s64 latency_req_ns)
{
	int i;

	for (i = 1; i < drv->state_count; i++) {
		u64 exit_latency_ns = (u64)drv->states[i].exit_latency * NSEC_PER_USEC;

		if (dev->states_usage[i].disable)
			continue;
		if (exit_latency_ns > latency_req_ns)
			continue;
		return i;
	}
	return 0;
}

/*
 * Cached wrapper around nap_find_min_valid_state().
 */
static inline int nap_get_min_valid_state(struct nap_cpu_data *d,
					  struct cpuidle_driver *drv,
					  struct cpuidle_device *dev,
					  s64 latency_req_ns)
{
	if (unlikely(latency_req_ns != d->cached_min_state_latency ||
		     time_after(jiffies,
				d->cached_min_state_jiffies +
				NAP_MIN_STATE_REFRESH_JIFFIES))) {
		d->cached_min_state = nap_find_min_valid_state(drv, dev,
							       latency_req_ns);
		d->cached_min_state_latency = latency_req_ns;
		d->cached_min_state_jiffies = jiffies;
	}
	return d->cached_min_state;
}

static int nap_select(struct cpuidle_driver *drv,
		      struct cpuidle_device *dev)
{
	struct nap_cpu_data *d = this_cpu_ptr(&nap_data);
	s64 latency_req_ns;
	u64 sleep_length_ns;
	u64 min_target_residency_ns;
	int idx, min_state;

	if (unlikely(drv->state_count <= 1))
		return 0;

	latency_req_ns = (s64)pm_qos_request(PM_QOS_CPU_DMA_LATENCY) * NSEC_PER_USEC;
	sleep_length_ns = ktime_to_ns(tick_nohz_get_sleep_length());
	min_state = nap_get_min_valid_state(d, drv, dev, latency_req_ns);

	/*
	 * Fast path: when no C-state can amortize its target residency
	 * within the predicted sleep length, the answer is deterministically
	 * POLL.  Skip NN inference and feature extraction entirely.
	 */
	min_target_residency_ns = min_state > 0 ? (u64)drv->states[min_state].target_residency * NSEC_PER_USEC : 0;
	if (min_state == 0 || sleep_length_ns < min_target_residency_ns) {
		d->last_selected_idx = 0;
		d->short_circuited = true;
		d->stats.total_selects++;
		return 0;
	}

	d->short_circuited = false;

	if (likely(may_use_simd())) {
		kernel_neon_begin();
		idx = nap_fpu_select(drv, dev, d);
		kernel_neon_end();

		if (idx < 0)
			idx = nap_fallback_heuristic(drv, dev);
	} else {
		idx = nap_fallback_heuristic(drv, dev);
	}

	d->last_selected_idx = idx;
	d->stats.total_selects++;

	return idx;
}

static void nap_reflect(struct cpuidle_device *dev, int index)
{
	struct nap_cpu_data *d = this_cpu_ptr(&nap_data);
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	u64 measured_ns = (u64)dev->last_residency * NSEC_PER_USEC;
	u64 target_residency_ns;

	if (unlikely(!drv))
		return;

	/*
	 * Short-circuited POLL: the NN was not invoked for this idle, so
	 * the residency is not part of its training distribution and must
	 * not feed the floor histogram or the weight update.
	 */
	if (d->short_circuited) {
		d->stats.total_residency_ns += measured_ns;
		return;
	}

	nap_history_update(d, measured_ns);

	d->last_prediction_error = d->last_predicted_ns - (s64)measured_ns;
	nap_update_external_signals(d);

	/* Every idle provides a fresh residency for the floor and reliability EMAs */
	d->learn_actual_ns = measured_ns;
	d->have_sample = true;

	/*
	 * Throttle the weight update.
	 */
	if (++d->learn_counter >= d->learn_interval &&
	    time_after_eq(jiffies,
			  d->last_learn_jiffies + d->learn_jiffies_min)) {
		d->learn_counter = 0;
		d->last_learn_jiffies = jiffies;
		d->needs_learn = true;
	}

	d->stats.total_residency_ns += measured_ns;
	target_residency_ns = (u64)drv->states[index].target_residency * NSEC_PER_USEC;
	if (index > 0 && measured_ns < target_residency_ns)
		d->stats.overshoot_count++;
}

static int nap_enable(struct cpuidle_driver *drv,
		      struct cpuidle_device *dev)
{
	struct nap_cpu_data *d = per_cpu_ptr(&nap_data, dev->cpu);

	memset(d, 0, sizeof(*d));

	/*
	 * Defer weight initialization to the first nap_select() NEON path
	 * via reset_pending.
	 */
	WRITE_ONCE(nap_cached_drv, drv);
	d->learning_rate_millths  = NAP_DEFAULT_LR_MILLTHS;
	d->learn_interval = NAP_DEFAULT_INTERVAL;
	d->max_grad_norm_millths  = NAP_DEFAULT_CLAMP_MILLTHS;
	d->conf_millths = NAP_DEFAULT_CONF_MILLTHS;

	d->cached_min_state_latency = S64_MIN;
	d->cached_min_state_jiffies = jiffies - NAP_MIN_STATE_REFRESH_JIFFIES;
	d->learn_jiffies_min = 1;

	d->reset_pending = true;

	return 0;
}

static void nap_disable(struct cpuidle_driver *drv,
			struct cpuidle_device *dev)
{
	WRITE_ONCE(nap_cached_drv, NULL);
}

/* ================================================================
 * sysfs interface  (/sys/devices/system/cpu/cpuidle/nap/)
 * ================================================================ */

static ssize_t stats_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	int cpu, len = 0;
	u64 total_sel = 0, total_res = 0, total_under = 0, total_learn = 0;

	for_each_online_cpu(cpu) {
		struct nap_cpu_data *d = &per_cpu(nap_data, cpu);

		total_sel   += d->stats.total_selects;
		total_res   += d->stats.total_residency_ns;
		total_under += d->stats.overshoot_count;
		total_learn += d->stats.learn_count;
	}

	len += sysfs_emit_at(buf, len, "total_selects: %llu\n", total_sel);
	len += sysfs_emit_at(buf, len, "total_residency_ms: %llu\n",
			     div_u64(total_res, NSEC_PER_MSEC));
	len += sysfs_emit_at(buf, len, "overshoot_count: %llu\n", total_under);
	len += sysfs_emit_at(buf, len, "overshoot_rate_permil: %llu\n",
			     total_sel ? div_u64(total_under * 1000, total_sel) : 0);
	len += sysfs_emit_at(buf, len, "learn_count: %llu\n", total_learn);
	return len;
}

static ssize_t learning_rate_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	int cpu;

	cpu = cpumask_first(cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%u\n",
			  per_cpu(nap_data, cpu).learning_rate_millths);
}

static ssize_t learning_rate_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned int val;
	int cpu;

	if (kstrtouint(buf, 10, &val) || val == 0 || val > 100)
		return -EINVAL;

	for_each_online_cpu(cpu)
		per_cpu(nap_data, cpu).learning_rate_millths = val;

	return count;
}

static ssize_t learn_interval_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	int cpu;

	cpu = cpumask_first(cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%d\n",
			  per_cpu(nap_data, cpu).learn_interval);
}

static ssize_t learn_interval_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned int val;
	int cpu;

	if (kstrtouint(buf, 10, &val) || val == 0 || val > 10000)
		return -EINVAL;

	for_each_online_cpu(cpu)
		per_cpu(nap_data, cpu).learn_interval = val;

	return count;
}

static ssize_t reset_weights_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int cpu;

	if (!READ_ONCE(nap_cached_drv))
		return -ENODEV;

	if (sysfs_streq(buf, "all")) {
		for_each_online_cpu(cpu)
			per_cpu(nap_data, cpu).reset_pending = true;
		pr_info("nap: weight reset scheduled for all CPUs\n");
		return count;
	}

	return -EINVAL;
}

static ssize_t reset_stats_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int cpu;

	for_each_online_cpu(cpu)
		memset(&per_cpu(nap_data, cpu).stats, 0,
		       sizeof(struct nap_stats));

	return count;
}

static ssize_t confidence_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	int cpu;

	cpu = cpumask_first(cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%u\n",
			  per_cpu(nap_data, cpu).conf_millths);
}

static ssize_t confidence_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int val;
	int cpu;

	if (kstrtouint(buf, 10, &val) || val == 0 || val >= 1000)
		return -EINVAL;

	for_each_online_cpu(cpu)
		per_cpu(nap_data, cpu).conf_millths = val;

	return count;
}

static ssize_t version_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CPUIDLE_NAP_VERSION);
}

static ssize_t simd_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "neon\n");
}

static struct kobj_attribute version_attr        = __ATTR_RO(version);
static struct kobj_attribute simd_attr           = __ATTR_RO(simd);
static struct kobj_attribute stats_attr          = __ATTR_RO(stats);
static struct kobj_attribute learning_rate_attr  = __ATTR_RW(learning_rate);
static struct kobj_attribute learn_interval_attr = __ATTR_RW(learn_interval);
static struct kobj_attribute confidence_attr     = __ATTR_RW(confidence);
static struct kobj_attribute reset_weights_attr  = __ATTR_WO(reset_weights);
static struct kobj_attribute reset_stats_attr    = __ATTR_WO(reset_stats);

static struct attribute *nap_attrs[] = {
	&version_attr.attr,
	&simd_attr.attr,
	&stats_attr.attr,
	&learning_rate_attr.attr,
	&learn_interval_attr.attr,
	&confidence_attr.attr,
	&reset_weights_attr.attr,
	&reset_stats_attr.attr,
	NULL,
};

static const struct attribute_group nap_attr_group = {
	.attrs = nap_attrs,
};

static struct kobject *cpuidle_kobj;

int nap_sysfs_init(void)
{
	int ret;

	if (!cpu_subsys.dev_root)
		return -ENODEV;

	cpuidle_kobj = kobject_create_and_add("nap", &cpu_subsys.dev_root->kobj);
	if (!cpuidle_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(cpuidle_kobj, &nap_attr_group);
	if (ret) {
		kobject_put(cpuidle_kobj);
		cpuidle_kobj = NULL;
	}
	return ret;
}

void nap_sysfs_exit(void)
{
	if (cpuidle_kobj) {
		sysfs_remove_group(cpuidle_kobj, &nap_attr_group);
		kobject_put(cpuidle_kobj);
		cpuidle_kobj = NULL;
	}
}

/* ================================================================
 * Governor registration
 * ================================================================ */

static struct cpuidle_governor nap_governor = {
	.name    = "nap",
	.rating  = 35,
	.enable  = nap_enable,
	.disable = nap_disable,
	.select  = nap_select,
	.reflect = nap_reflect,
};

static int __init nap_init(void)
{
	int ret;

	ret = nap_sysfs_init();
	if (ret)
		pr_warn("nap: sysfs init failed: %d (continuing without sysfs)\n", ret);

	ret = cpuidle_register_governor(&nap_governor);
	if (ret) {
		pr_err("nap: register_governor failed: %d\n", ret);
		nap_sysfs_exit();
		return ret;
	}

	pr_info("%s v%s by %s registered (rating=%u)\n",
	       CPUIDLE_NAP_PROGNAME, CPUIDLE_NAP_VERSION,
	       CPUIDLE_NAP_AUTHOR, nap_governor.rating);
	return 0;
}
postcore_initcall(nap_init);

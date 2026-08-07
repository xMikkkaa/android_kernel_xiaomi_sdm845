// SPDX-License-Identifier: GPL-2.0
/*
 * High-Yield Dynamic Render Affinity (HYDRA) Thread Optimizer
 * Copyright (C) 2026 xMikkkaa
 *
 * HYDRA optimizes game rendering threads by applying nice boosts and
 * big-core affinity. It is activated on-demand via sysctl and operates
 * entirely isolated from the core scheduler (fair.c, core.c, rt.c).
 *
 * Integration points (existing kernel files touched):
 *   - kernel/sched/Makefile: obj-$(CONFIG_SCHED_HYDRA) += hydra.o
 *   - init/Kconfig: CONFIG_SCHED_HYDRA entry
 *
 * All other logic is self-contained in this file + hydra.h.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sysctl.h>
#include <linux/cpumask.h>
#include <linux/sched/hydra.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/capability.h>
#include <linux/ratelimit.h>
#include <linux/cpufreq.h>
#include <linux/seq_file.h>
#include <trace/events/sched.h>
#include "sched.h"

/* ======================== Global sysctl variables ======================== */

int __read_mostly sched_hydra_enable = 1;
DEFINE_STATIC_KEY_TRUE(sched_hydra_enable_key);

int sched_hydra_pid;
int sched_hydra_nice = -10;
int sched_hydra_smart_mode = 2; /* 0: Off, 1: Hard Pin, 2: Smart Logic */
int sched_hydra_throttle_freq = 1400000;
int sched_hydra_heavy_util = 300;
int sched_hydra_light_util = 100;
int sched_hydra_cluster_depth = 0;

#ifdef CONFIG_SCHED_HYDRA_DEBUG
char hydra_debug_fake_clusters[128] = "";
#endif

/* ======================== Internal state ================================= */

static struct hydra_thread_state hydra_threads[HYDRA_MAX_TRACKED_THREADS];
static int hydra_thread_count;
static DEFINE_MUTEX(hydra_mutex);
static struct task_struct *hydra_target_buffer[HYDRA_MAX_TRACKED_THREADS];

static cpumask_t hydra_clusters[HYDRA_MAX_CLUSTERS];
static unsigned long hydra_cluster_capacity[HYDRA_MAX_CLUSTERS];
static int hydra_cluster_count;
static DEFINE_MUTEX(hydra_topo_mutex);

static struct workqueue_struct *hydra_wq;

static struct work_struct hydra_fork_work;
static struct work_struct hydra_exit_work;
static atomic_t hydra_exit_pid = ATOMIC_INIT(0);

static struct delayed_work hydra_smart_dwork;
static bool hydra_smart_running;

/* ======================== Smart worker lifecycle ========================= */

static void start_smart_worker(void)
{
	if (!hydra_smart_running && sched_hydra_smart_mode == 2) {
		hydra_smart_running = true;
		schedule_delayed_work(&hydra_smart_dwork, 0);
	}
}

static void stop_smart_worker(void)
{
	if (hydra_smart_running) {
		hydra_smart_running = false;
		cancel_delayed_work_sync(&hydra_smart_dwork);
	}
}

/* ======================== Topology detection ============================= */

/*
 * NOTE: Topology detection assumes the cluster masks (hydra_clusters) are
 * static. If CPU hotplug removes cores, this mask may contain offline CPUs.
 * The cpumask_and(&allowed_mask, ..., cpu_online_mask) operation at usage
 * sites mitigates this, but a full cpuhp_state callback would be needed for
 * perfect hotplug awareness.
 */
static unsigned long hydra_get_cpu_capacity(int cpu)
{
#ifdef CONFIG_SCHED_HYDRA_DEBUG
	if (hydra_debug_fake_clusters[0] != '\0') {
		char *p = hydra_debug_fake_clusters;
		unsigned long caps[HYDRA_MAX_CLUSTERS + 4] = {0};
		int count = 0, i;

		for (i = 0; i < ARRAY_SIZE(caps) && p && *p; i++) {
			caps[i] = simple_strtoul(p, &p, 10);
			if (*p == ',')
				p++;
			count++;
		}
		
		if (count > 0)
			return caps[cpu % count] ? caps[cpu % count] : 1024;
	}
#endif
	return capacity_orig_of(cpu);
}

static void hydra_detect_clusters(void)
{
	int cpu, i, j;
	int temp_count = 0;
	cpumask_t temp_masks[16];
	unsigned long temp_caps[16];

	mutex_lock(&hydra_topo_mutex);
	if (hydra_cluster_count > 0) {
		mutex_unlock(&hydra_topo_mutex);
		return;
	}

	for (i = 0; i < 16; i++) {
		cpumask_clear(&temp_masks[i]);
		temp_caps[i] = 0;
	}

	for_each_possible_cpu(cpu) {
		unsigned long cap = hydra_get_cpu_capacity(cpu);
		bool found = false;

		for (i = 0; i < temp_count; i++) {
			unsigned long max_c = max(cap, temp_caps[i]);
			unsigned long min_c = min(cap, temp_caps[i]);
			if (max_c - min_c <= (max_c * 6) / 100) {
				cpumask_set_cpu(cpu, &temp_masks[i]);
				temp_caps[i] = max_c;
				found = true;
				break;
			}
		}
		if (!found && temp_count < 16) {
			cpumask_set_cpu(cpu, &temp_masks[temp_count]);
			temp_caps[temp_count] = cap;
			temp_count++;
		}
	}

	for (i = 0; i < temp_count - 1; i++) {
		for (j = i + 1; j < temp_count; j++) {
			if (temp_caps[i] > temp_caps[j]) {
				unsigned long tmp_cap = temp_caps[i];
				cpumask_t tmp_mask;
				
				temp_caps[i] = temp_caps[j];
				temp_caps[j] = tmp_cap;
				
				cpumask_copy(&tmp_mask, &temp_masks[i]);
				cpumask_copy(&temp_masks[i], &temp_masks[j]);
				cpumask_copy(&temp_masks[j], &tmp_mask);
			}
		}
	}

	while (temp_count > HYDRA_MAX_CLUSTERS) {
		cpumask_or(&temp_masks[1], &temp_masks[0], &temp_masks[1]);
		for (i = 0; i < temp_count - 1; i++) {
			cpumask_copy(&temp_masks[i], &temp_masks[i + 1]);
			temp_caps[i] = temp_caps[i + 1];
		}
		temp_count--;
	}

	if (temp_count == 1 && cpumask_weight(&temp_masks[0]) == num_possible_cpus()) {
		pr_warn_ratelimited("hydra: cluster detection unreliable, retrying later\n");
	} else {
		for (i = 0; i < temp_count; i++) {
			cpumask_copy(&hydra_clusters[i], &temp_masks[i]);
			hydra_cluster_capacity[i] = temp_caps[i];
			pr_info("hydra: tier %d (cap ~%lu) mask: %*pbl\n",
				i, temp_caps[i], cpumask_pr_args(&hydra_clusters[i]));
		}
		hydra_cluster_count = temp_count;
	}
	mutex_unlock(&hydra_topo_mutex);
}

/* ======================== Thread matching ================================ */

static bool hydra_match_thread(struct task_struct *t)
{
	int i;

	for (i = 0; i < HYDRA_NUM_COMM_PATTERNS; i++) {
		if (strnstr(t->comm, hydra_comm_patterns[i], TASK_COMM_LEN))
			return true;
	}

	return false;
}

/* ======================== Revert logic =================================== */

static bool hydra_revert_all_threads(bool clear_state, int expected_pid)
{
	int i;
	struct task_struct *t;

	stop_smart_worker();

	mutex_lock(&hydra_mutex);

	/* Verify expected_pid inside mutex to prevent lock-race on fast PID switch */
	if (expected_pid > 0 && READ_ONCE(sched_hydra_pid) != expected_pid) {
		mutex_unlock(&hydra_mutex);
		return false;
	}

	for (i = 0; i < hydra_thread_count; i++) {
		struct hydra_thread_state *state = &hydra_threads[i];

		rcu_read_lock();
		t = find_task_by_pid_ns(state->tid, &init_pid_ns);
		if (t) {
			if (t->start_time != state->start_time) {
				/* TID reused by a different task — skip */
				t = NULL;
			} else {
				get_task_struct(t);
			}
		}
		rcu_read_unlock();

		if (t) {
			int err;
			set_user_nice(t, state->original_nice);
			err = set_cpus_allowed_ptr(t, &state->original_mask);
			if (err)
				pr_warn_ratelimited("hydra: affinity set failed for tid %d (err=%d)\n",
						    t->pid, err);
			put_task_struct(t);
		}
	}

	if (clear_state)
		hydra_thread_count = 0;

	mutex_unlock(&hydra_mutex);
	return true;
}

/* ======================== Optimize threads ================================ */

static void hydra_optimize_threads(pid_t pid)
{
	struct task_struct *p, *t;
	int target_count = 0;
	int i, start, effective_depth;
	cpumask_t allowed_mask;
	bool has_big_cores;

	if (pid <= 0)
		return;

	hydra_detect_clusters();

	mutex_lock(&hydra_topo_mutex);
	cpumask_clear(&allowed_mask);
	effective_depth = (sched_hydra_cluster_depth == 0)
		? max(1, hydra_cluster_count - 1)
		: sched_hydra_cluster_depth;
	start = max(0, hydra_cluster_count - effective_depth);
	for (i = start; i < hydra_cluster_count; i++)
		cpumask_or(&allowed_mask, &allowed_mask, &hydra_clusters[i]);
	cpumask_and(&allowed_mask, &allowed_mask, cpu_online_mask);
	mutex_unlock(&hydra_topo_mutex);
	has_big_cores = !cpumask_empty(&allowed_mask);

	rcu_read_lock();
	p = find_task_by_pid_ns(pid, &init_pid_ns);
	if (!p) {
		rcu_read_unlock();
		return;
	}
	get_task_struct(p);
	rcu_read_unlock();

	/*
	 * get_task_struct(t) under read_lock(&tasklist_lock) is safe:
	 * it performs an atomic_inc() on t->usage which is non-blocking.
	 * The read_lock is required for safe for_each_thread() iteration.
	 */
	mutex_lock(&hydra_mutex);
	read_lock(&tasklist_lock);
	for_each_thread(p, t) {
		if (hydra_match_thread(t)) {
			if (target_count < HYDRA_MAX_TRACKED_THREADS) {
				get_task_struct(t);
				hydra_target_buffer[target_count++] = t;
			} else {
				pr_warn_ratelimited("hydra: thread cap reached (%d), dropping thread\n",
						    HYDRA_MAX_TRACKED_THREADS);
			}
		}
	}
	read_unlock(&tasklist_lock);
	put_task_struct(p);

	/*
	 * Early return if no threads matched — safe because p was already
	 * released above and hydra_target_buffer[] is empty.
	 */
	if (target_count == 0) {
		mutex_unlock(&hydra_mutex);
		return;
	}

	/* Prune dead or TID-reused threads to free up slots (O(1) swap-and-pop) */
	for (i = 0; i < hydra_thread_count; i++) {
		struct hydra_thread_state *state = &hydra_threads[i];
		bool dead_or_reused = false;

		rcu_read_lock();
		t = find_task_by_pid_ns(state->tid, &init_pid_ns);
		if (!t || t->tgid != pid || t->start_time != state->start_time)
			dead_or_reused = true;
		rcu_read_unlock();

		if (dead_or_reused) {
			hydra_thread_count--;
			if (i < hydra_thread_count) {
				hydra_threads[i] = hydra_threads[hydra_thread_count];
				i--; /* Re-examine swapped element */
			}
		}
	}

	for (i = 0; i < target_count; i++) {
		bool tracked = false;
		int j;

		t = hydra_target_buffer[i];

		/* Check if thread is already tracked */
		for (j = 0; j < hydra_thread_count; j++) {
			if (hydra_threads[j].tid == t->pid &&
			    hydra_threads[j].start_time == t->start_time) {
				tracked = true;
				break;
			}
		}

		if (!tracked && hydra_thread_count < HYDRA_MAX_TRACKED_THREADS) {
			unsigned long flags;
			struct hydra_thread_state *state =
				&hydra_threads[hydra_thread_count++];
			state->tid = t->pid;
			state->start_time = t->start_time;
			state->original_nice = task_nice(t);

			raw_spin_lock_irqsave(&t->pi_lock, flags);
			cpumask_copy(&state->original_mask,
				     &hydra_cpus_allowed(t));
			raw_spin_unlock_irqrestore(&t->pi_lock, flags);
		}

		/* Nice boost is always applied regardless of cpumask availability */
		set_user_nice(t, sched_hydra_nice);

		/* Cpumask assignment only when big cores are online */
		if (sched_hydra_smart_mode == 1 && has_big_cores) {
			int err = set_cpus_allowed_ptr(t, &allowed_mask);
			if (err)
				pr_warn_ratelimited("hydra: affinity set failed for tid %d (err=%d)\n",
						    t->pid, err);
		}

		put_task_struct(t);
	}
	mutex_unlock(&hydra_mutex);

	if (sched_hydra_smart_mode == 2)
		start_smart_worker();

	pr_info_ratelimited("hydra: optimized game pid %d (%d threads)\n",
			    pid, target_count);
}

/* ======================== Smart worker =================================== */

static void hydra_smart_work_fn(struct work_struct *work)
{
	int i, start, effective_depth;
	struct task_struct *t;
	unsigned int big_core_freq = 0;
	bool is_throttled = false;
	cpumask_t allowed_mask;
	int pid = READ_ONCE(sched_hydra_pid);

	if (pid <= 0 ||
	    !static_branch_likely(&sched_hydra_enable_key))
		goto end;

	if (sched_hydra_smart_mode != 2)
		goto end;

	mutex_lock(&hydra_topo_mutex);
	/* Get frequency of the highest big core tier for thermal check */
	if (hydra_cluster_count > 0) {
		int cpu = cpumask_first(&hydra_clusters[hydra_cluster_count - 1]);

		big_core_freq = cpufreq_quick_get(cpu);
	}

	cpumask_clear(&allowed_mask);
	effective_depth = (sched_hydra_cluster_depth == 0)
		? max(1, hydra_cluster_count - 1)
		: sched_hydra_cluster_depth;
	start = max(0, hydra_cluster_count - effective_depth);
	for (i = start; i < hydra_cluster_count; i++)
		cpumask_or(&allowed_mask, &allowed_mask, &hydra_clusters[i]);
	cpumask_and(&allowed_mask, &allowed_mask, cpu_online_mask);
	mutex_unlock(&hydra_topo_mutex);

	if (big_core_freq > 0 && big_core_freq < sched_hydra_throttle_freq)
		is_throttled = true;

	mutex_lock(&hydra_mutex);
	for (i = 0; i < hydra_thread_count; i++) {
		struct hydra_thread_state *state = &hydra_threads[i];
		unsigned long util = 0;
		bool dead_or_reused = false;

		rcu_read_lock();
		t = find_task_by_pid_ns(state->tid, &init_pid_ns);
		if (t) {
			if (t->tgid == pid &&
			    t->start_time == state->start_time) {
				get_task_struct(t);
				util = task_util(t);
			} else {
				t = NULL;
				dead_or_reused = true;
			}
		} else {
			dead_or_reused = true;
		}
		rcu_read_unlock();

		/* O(1) swap-and-pop pruning for dead/reused threads */
		if (dead_or_reused) {
			hydra_thread_count--;
			if (i < hydra_thread_count) {
				hydra_threads[i] = hydra_threads[hydra_thread_count];
				i--;
			}
			continue;
		}

		if (t) {
			int err = 0;
			if (is_throttled) {
				/* Thermal protection: release big core pinning */
				err = set_cpus_allowed_ptr(t, &state->original_mask);
			} else {
				/*
				 * Hysteresis: only change cpumask at boundaries.
				 * Between light_util and heavy_util, thread keeps
				 * its current state to prevent cpumask thrashing.
				 */
				if (util > sched_hydra_heavy_util) {
					if (!cpumask_empty(&allowed_mask))
						err = set_cpus_allowed_ptr(t,
							&allowed_mask);
				} else if (util < sched_hydra_light_util) {
					err = set_cpus_allowed_ptr(t,
						&state->original_mask);
				}
			}
			if (err)
				pr_warn_ratelimited("hydra: affinity set failed for tid %d (err=%d)\n",
						    t->pid, err);
			put_task_struct(t);
		}
	}
	mutex_unlock(&hydra_mutex);

end:
	if (hydra_smart_running)
		schedule_delayed_work(&hydra_smart_dwork,
				      msecs_to_jiffies(500));
}

/* ======================== Tracepoint work handlers ======================= */

static void hydra_fork_work_fn(struct work_struct *work)
{
	int pid = READ_ONCE(sched_hydra_pid);

	if (pid > 0 && static_branch_likely(&sched_hydra_enable_key))
		hydra_optimize_threads(pid);
}

static void hydra_exit_work_fn(struct work_struct *work)
{
	int exit_pid = atomic_xchg(&hydra_exit_pid, 0);

	if (exit_pid > 0) {
		if (hydra_revert_all_threads(true, exit_pid)) {
			/*
			 * Use cmpxchg to prevent wiping a new PID set by
			 * userspace while the exit worker was queued.
			 */
			if (cmpxchg(&sched_hydra_pid, exit_pid, 0) == exit_pid)
				pr_info("hydra: auto-reverted on game exit (pid %d)\n",
					exit_pid);
		}
	}
}

/* ======================== Tracepoint probes ============================== */

static void hydra_queue_work(struct work_struct *work)
{
	if (hydra_wq)
		queue_work(hydra_wq, work);
	else
		schedule_work(work);
}

static void probe_sched_process_fork(void *ignore,
				      struct task_struct *parent,
				      struct task_struct *child)
{
	int pid = READ_ONCE(sched_hydra_pid);

	if (pid > 0 && parent->tgid == pid)
		hydra_queue_work(&hydra_fork_work);
}

static void probe_sched_process_exit(void *ignore, struct task_struct *p)
{
	int pid = READ_ONCE(sched_hydra_pid);

	if (pid > 0 && p->tgid == pid && p->pid == pid) {
		atomic_set(&hydra_exit_pid, pid);
		hydra_queue_work(&hydra_exit_work);
	}
}

/* ======================== Sysctl handlers ================================ */

static int sched_hydra_pid_handler(struct ctl_table *table, int write,
				   void __user *buffer, size_t *lenp,
				   loff_t *ppos)
{
	int ret;
	/*
	 * READ_ONCE for old_pid: sysctl framework serializes writes to
	 * the same entry, but we use READ_ONCE for correctness annotation.
	 */
	int old_pid = READ_ONCE(sched_hydra_pid);

	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (sched_hydra_pid != old_pid) {
		if (old_pid > 0)
			hydra_revert_all_threads(true, 0);

		if (sched_hydra_pid > 0 &&
		    static_branch_likely(&sched_hydra_enable_key))
			hydra_optimize_threads(sched_hydra_pid);
	}

	return 0;
}

static int sched_hydra_enable_handler(struct ctl_table *table, int write,
				      void __user *buffer, size_t *lenp,
				      loff_t *ppos)
{
	int ret;
	int old = sched_hydra_enable;

	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (sched_hydra_enable != old) {
		if (sched_hydra_enable) {
			static_branch_enable(&sched_hydra_enable_key);
			if (sched_hydra_pid > 0)
				hydra_optimize_threads(sched_hydra_pid);
		} else {
			static_branch_disable(&sched_hydra_enable_key);
			if (sched_hydra_pid > 0)
				hydra_revert_all_threads(true, 0);
		}
	}

	return 0;
}

static int sched_hydra_nice_handler(struct ctl_table *table, int write,
				    void __user *buffer, size_t *lenp,
				    loff_t *ppos)
{
	int ret;
	int old_nice = sched_hydra_nice;

	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (sched_hydra_nice < -15)
		sched_hydra_nice = -15;
	else if (sched_hydra_nice > -1)
		sched_hydra_nice = -1;

	if (sched_hydra_nice != old_nice && sched_hydra_pid > 0 &&
	    static_branch_likely(&sched_hydra_enable_key))
		hydra_optimize_threads(sched_hydra_pid);

	return 0;
}

static int sched_hydra_smart_mode_handler(struct ctl_table *table, int write,
					  void __user *buffer, size_t *lenp,
					  loff_t *ppos)
{
	int ret;
	int old_mode = sched_hydra_smart_mode;

	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (sched_hydra_smart_mode < 0)
		sched_hydra_smart_mode = 0;
	else if (sched_hydra_smart_mode > 2)
		sched_hydra_smart_mode = 2;

	if (sched_hydra_smart_mode != old_mode) {
		/* Always stop smart worker when leaving mode 2 */
		if (old_mode == 2)
			stop_smart_worker();

		if (sched_hydra_smart_mode == 0) {
			/* Full revert but keep state for re-enable */
			hydra_revert_all_threads(false, 0);
		} else if (sched_hydra_smart_mode == 1) {
			if (sched_hydra_pid > 0 &&
			    static_branch_likely(&sched_hydra_enable_key))
				hydra_optimize_threads(sched_hydra_pid);
		} else if (sched_hydra_smart_mode == 2) {
			if (sched_hydra_pid > 0 &&
			    static_branch_likely(&sched_hydra_enable_key)) {
				hydra_optimize_threads(sched_hydra_pid);
				start_smart_worker();
			}
		}
	}

	return 0;
}

static int sched_hydra_simple_int_handler(struct ctl_table *table, int write,
					  void __user *buffer, size_t *lenp,
					  loff_t *ppos)
{
	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	return proc_dointvec(table, write, buffer, lenp, ppos);
}

/*
 * Dedicated handler for util thresholds.
 * Enforces the invariant: hydra_light_util <= hydra_heavy_util.
 */
static int sched_hydra_util_handler(struct ctl_table *table, int write,
				    void __user *buffer, size_t *lenp,
				    loff_t *ppos)
{
	int ret;

	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	mutex_lock(&hydra_mutex);
	/* Clamp to valid range */
	if (sched_hydra_heavy_util < 0)
		sched_hydra_heavy_util = 0;
	else if (sched_hydra_heavy_util > 1024)
		sched_hydra_heavy_util = 1024;

	if (sched_hydra_light_util < 0)
		sched_hydra_light_util = 0;
	else if (sched_hydra_light_util > 1024)
		sched_hydra_light_util = 1024;

	/* Enforce light <= heavy invariant */
	if (sched_hydra_light_util > sched_hydra_heavy_util)
		sched_hydra_light_util = sched_hydra_heavy_util;
	mutex_unlock(&hydra_mutex);

	return 0;
}

/*
 * Read-only sysctl for observability.
 * Shows: tracked threads, active mode, throttle state, PID, version.
 */
static int sched_hydra_stats_handler(struct ctl_table *table, int write,
				     void __user *buffer, size_t *lenp,
				     loff_t *ppos)
{
	char stats_buf[1024];
	struct ctl_table tmp_table;
	unsigned int big_core_freq = 0;
	bool is_throttled = false;
	int count = 0;
	int i;

	if (write)
		return -EPERM;

	mutex_lock(&hydra_topo_mutex);
	if (hydra_cluster_count > 0) {
		int cpu = cpumask_first(&hydra_clusters[hydra_cluster_count - 1]);

		big_core_freq = cpufreq_quick_get(cpu);
	}
	mutex_unlock(&hydra_topo_mutex);

	if (big_core_freq > 0 && big_core_freq < sched_hydra_throttle_freq)
		is_throttled = true;

	count += snprintf(stats_buf + count, sizeof(stats_buf) - count,
			 "version=%s\n"
			 "enabled=%d\n"
			 "pid=%d\n"
			 "mode=%d\n"
			 "depth=%d\n"
			 "tracked_threads=%d\n"
			 "throttled=%d\n"
			 "big_core_freq=%u\n",
			 SCHED_HYDRA_VERSION,
			 sched_hydra_enable,
			 sched_hydra_pid,
			 sched_hydra_smart_mode,
			 sched_hydra_cluster_depth,
			 READ_ONCE(hydra_thread_count),
			 is_throttled ? 1 : 0,
			 big_core_freq);

	mutex_lock(&hydra_topo_mutex);
	count += snprintf(stats_buf + count, sizeof(stats_buf) - count,
			  "clusters: %d\n", hydra_cluster_count);
	for (i = 0; i < hydra_cluster_count; i++) {
		count += snprintf(stats_buf + count, sizeof(stats_buf) - count,
				  "tier %d: cap=%lu cpus=%d mask=%*pbl\n",
				  i, hydra_cluster_capacity[i],
				  cpumask_weight(&hydra_clusters[i]),
				  cpumask_pr_args(&hydra_clusters[i]));
	}
	mutex_unlock(&hydra_topo_mutex);

	tmp_table = *table;
	tmp_table.data = stats_buf;
	tmp_table.maxlen = count;

	return proc_dostring(&tmp_table, write, buffer, lenp, ppos);
}

static int sched_hydra_depth_handler(struct ctl_table *table, int write,
				     void __user *buffer, size_t *lenp,
				     loff_t *ppos)
{
	int ret;
	int old = sched_hydra_cluster_depth;
	int max_depth;

	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	mutex_lock(&hydra_topo_mutex);
	max_depth = max(1, hydra_cluster_count - 1);
	mutex_unlock(&hydra_topo_mutex);

	if (sched_hydra_cluster_depth < 0)
		sched_hydra_cluster_depth = 0;
	else if (sched_hydra_cluster_depth > max_depth)
		sched_hydra_cluster_depth = max_depth;

	if (sched_hydra_cluster_depth != old && sched_hydra_pid > 0 &&
	    static_branch_likely(&sched_hydra_enable_key)) {
		if (sched_hydra_smart_mode == 2)
			start_smart_worker();
		else if (sched_hydra_smart_mode == 1)
			hydra_optimize_threads(sched_hydra_pid);
	}

	return 0;
}

#ifdef CONFIG_SCHED_HYDRA_DEBUG
static int sched_hydra_debug_handler(struct ctl_table *table, int write,
				     void __user *buffer, size_t *lenp,
				     loff_t *ppos)
{
	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	return proc_dostring(table, write, buffer, lenp, ppos);
}
#endif

/* ======================== Sysctl table =================================== */

static struct ctl_table hydra_sysctls[] = {
	{
		.procname	= "hydra_enable",
		.data		= &sched_hydra_enable,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_enable_handler,
	},
	{
		.procname	= "hydra_pid",
		.data		= &sched_hydra_pid,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_pid_handler,
	},
	{
		.procname	= "hydra_nice",
		.data		= &sched_hydra_nice,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_nice_handler,
	},
	{
		.procname	= "hydra_smart_mode",
		.data		= &sched_hydra_smart_mode,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_smart_mode_handler,
	},
	{
		.procname	= "hydra_throttle_freq",
		.data		= &sched_hydra_throttle_freq,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_simple_int_handler,
	},
	{
		.procname	= "hydra_heavy_util",
		.data		= &sched_hydra_heavy_util,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_util_handler,
	},
	{
		.procname	= "hydra_light_util",
		.data		= &sched_hydra_light_util,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_util_handler,
	},
	{
		.procname	= "hydra_stats",
		.data		= NULL,
		.maxlen		= 0,
		.mode		= 0444,
		.proc_handler	= sched_hydra_stats_handler,
	},
	{
		.procname	= "hydra_cluster_depth",
		.data		= &sched_hydra_cluster_depth,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_depth_handler,
	},
#ifdef CONFIG_SCHED_HYDRA_DEBUG
	{
		.procname	= "hydra_debug_fake_clusters",
		.data		= hydra_debug_fake_clusters,
		.maxlen		= sizeof(hydra_debug_fake_clusters),
		.mode		= 0644,
		.proc_handler	= sched_hydra_debug_handler,
	},
#endif
	{ }
};

static struct ctl_table_header *hydra_sysctl_header;

/* ======================== Init =========================================== */

static int __init sched_init_hydra(void)
{
	hydra_detect_clusters();

	hydra_wq = alloc_workqueue("hydra", WQ_HIGHPRI, 0);
	if (!hydra_wq)
		pr_err("hydra: failed to allocate high-priority workqueue, falling back to system_wq\n");

	INIT_WORK(&hydra_fork_work, hydra_fork_work_fn);
	INIT_WORK(&hydra_exit_work, hydra_exit_work_fn);
	INIT_DELAYED_WORK(&hydra_smart_dwork, hydra_smart_work_fn);

	register_trace_sched_process_fork(probe_sched_process_fork, NULL);
	register_trace_sched_process_exit(probe_sched_process_exit, NULL);

	hydra_sysctl_header = register_sysctl("kernel", hydra_sysctls);
	pr_info("hydra: " SCHED_HYDRA_PROGNAME " v" SCHED_HYDRA_VERSION
		" initialized.\n");
	return 0;
}
late_initcall(sched_init_hydra);

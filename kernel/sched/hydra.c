/*
 * High-Yield Dynamic Render Affinity (HYDRA) Thread Optimizer
 * Copyright (C) 2026 xMikkkaa
 * Licensed under the GNU General Public License version 2 (GPLv2)
 */
#include <linux/kernel.h>
#include <linux/module.h>
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
#include <trace/events/sched.h>
#include "sched.h"

int __read_mostly sched_hydra_enable = 1;
DEFINE_STATIC_KEY_TRUE(sched_hydra_enable_key);

int sched_hydra_pid = 0;
int sched_hydra_nice = -10;
int sched_hydra_smart_mode = 2; /* 0: Off, 1: Hard Pin, 2: Smart Logic */
int sched_hydra_throttle_freq = 1400000;
int sched_hydra_heavy_util = 300;
int sched_hydra_light_util = 100;

static struct hydra_thread_state hydra_threads[HYDRA_MAX_TRACKED_THREADS];
static int hydra_thread_count = 0;
static DEFINE_MUTEX(hydra_mutex);

static cpumask_t hydra_big_cores;
static bool hydra_topology_detected = false;

static struct work_struct hydra_fork_work;
static struct work_struct hydra_exit_work;
static atomic_t hydra_exit_pending = ATOMIC_INIT(0);

static struct delayed_work hydra_smart_dwork;
static bool hydra_smart_running = false;

static void start_smart_worker(void)
{
	if (!hydra_smart_running && sched_hydra_smart_mode == 2) {
		hydra_smart_running = true;
		schedule_delayed_work(&hydra_smart_dwork, msecs_to_jiffies(500));
	}
}

static void stop_smart_worker(void)
{
	if (hydra_smart_running) {
		hydra_smart_running = false;
		cancel_delayed_work_sync(&hydra_smart_dwork);
	}
}

static void hydra_detect_big_cores(void)
{
	int cpu;
	unsigned long max_cap = 0;

	if (hydra_topology_detected)
		return;

	cpumask_clear(&hydra_big_cores);

	for_each_possible_cpu(cpu) {
		unsigned long cap = capacity_orig_of(cpu);
		if (cap > max_cap)
			max_cap = cap;
	}

	for_each_possible_cpu(cpu) {
		if (capacity_orig_of(cpu) == max_cap)
			cpumask_set_cpu(cpu, &hydra_big_cores);
	}

	hydra_topology_detected = true;
	pr_info("hydra: big core mask detected: %*pbl\n", cpumask_pr_args(&hydra_big_cores));
}

static bool hydra_match_thread(struct task_struct *t)
{
	char comm[TASK_COMM_LEN];
	
	get_task_comm(comm, t);

	if (strnstr(comm, "RenderThread", TASK_COMM_LEN) ||
	    strnstr(comm, "UnityMain", TASK_COMM_LEN) ||
	    strnstr(comm, "UnityGfx", TASK_COMM_LEN) ||
	    strnstr(comm, "TaskGraph", TASK_COMM_LEN) ||
	    strnstr(comm, "GameThread", TASK_COMM_LEN) ||
	    strnstr(comm, "adreno", TASK_COMM_LEN) ||
	    strnstr(comm, "glthread", TASK_COMM_LEN) ||
	    strnstr(comm, "kgsl", TASK_COMM_LEN) ||
	    strnstr(comm, "ANGLE", TASK_COMM_LEN) ||
	    strnstr(comm, "FrameWorker", TASK_COMM_LEN))
		return true;

	return false;
}

static void hydra_revert_all_threads(bool clear_state)
{
	int i;
	struct task_struct *t;

	stop_smart_worker();

	mutex_lock(&hydra_mutex);
	for (i = 0; i < hydra_thread_count; i++) {
		struct hydra_thread_state *state = &hydra_threads[i];
		
		rcu_read_lock();
		t = find_task_by_pid_ns(state->tid, &init_pid_ns);
		if (t)
			get_task_struct(t);
		rcu_read_unlock();

		if (t) {
			set_user_nice(t, state->original_nice);
			set_cpus_allowed_ptr(t, &state->original_mask);
			put_task_struct(t);
		}
	}
	if (clear_state)
		hydra_thread_count = 0;
	mutex_unlock(&hydra_mutex);
}

static void hydra_revert_cpumask_only(void)
{
	int i;
	struct task_struct *t;

	mutex_lock(&hydra_mutex);
	for (i = 0; i < hydra_thread_count; i++) {
		struct hydra_thread_state *state = &hydra_threads[i];
		
		rcu_read_lock();
		t = find_task_by_pid_ns(state->tid, &init_pid_ns);
		if (t)
			get_task_struct(t);
		rcu_read_unlock();

		if (t) {
			set_cpus_allowed_ptr(t, &state->original_mask);
			put_task_struct(t);
		}
	}
	mutex_unlock(&hydra_mutex);
}

static void hydra_optimize_threads(pid_t pid)
{
	struct task_struct *p, *t;
	struct task_struct *targets[HYDRA_MAX_TRACKED_THREADS];
	int target_count = 0;
	int i;
	cpumask_t allowed_mask;

	if (pid <= 0)
		return;

	hydra_detect_big_cores();

	cpumask_and(&allowed_mask, &hydra_big_cores, cpu_online_mask);
	if (cpumask_empty(&allowed_mask))
		return;

	rcu_read_lock();
	p = find_task_by_pid_ns(pid, &init_pid_ns);
	if (!p) {
		rcu_read_unlock();
		return;
	}
	get_task_struct(p);
	rcu_read_unlock();

	read_lock(&tasklist_lock);
	for_each_thread(p, t) {
		if (hydra_match_thread(t)) {
			if (target_count < HYDRA_MAX_TRACKED_THREADS) {
				get_task_struct(t);
				targets[target_count++] = t;
			}
		}
	}
	read_unlock(&tasklist_lock);
	put_task_struct(p);

	if (target_count == 0)
		return;

	mutex_lock(&hydra_mutex);
	
	/* Cleanup dead or reused threads to free up slots */
	for (i = 0; i < hydra_thread_count; i++) {
		struct hydra_thread_state *state = &hydra_threads[i];
		bool dead_or_reused = false;
		
		rcu_read_lock();
		t = find_task_by_pid_ns(state->tid, &init_pid_ns);
		if (!t || t->tgid != pid)
			dead_or_reused = true;
		rcu_read_unlock();

		if (dead_or_reused) {
			hydra_thread_count--;
			if (i < hydra_thread_count) {
				hydra_threads[i] = hydra_threads[hydra_thread_count];
				i--;
			}
		}
	}

	for (i = 0; i < target_count; i++) {
		bool tracked = false;
		int j;
		t = targets[i];
		
		for (j = 0; j < hydra_thread_count; j++) {
			if (hydra_threads[j].tid == t->pid) {
				tracked = true;
				break;
			}
		}

		if (!tracked && hydra_thread_count < HYDRA_MAX_TRACKED_THREADS) {
			struct hydra_thread_state *state = &hydra_threads[hydra_thread_count++];
			state->tid = t->pid;
			state->original_nice = task_nice(t);
			cpumask_copy(&state->original_mask, &t->cpus_allowed);
		}

		set_user_nice(t, sched_hydra_nice);
		
		if (sched_hydra_smart_mode == 1)
			set_cpus_allowed_ptr(t, &allowed_mask);

		put_task_struct(t);
	}
	mutex_unlock(&hydra_mutex);

	if (sched_hydra_smart_mode == 2)
		start_smart_worker();

	pr_info_ratelimited("hydra: optimized game pid %d (%d threads)\n", pid, target_count);
}

static void hydra_smart_work_fn(struct work_struct *work)
{
	int i;
	struct task_struct *t;
	unsigned int big_core_freq = 0;
	bool is_throttled = false;
	cpumask_t allowed_mask;

	if (sched_hydra_pid <= 0 || !static_branch_likely(&sched_hydra_enable_key))
		goto end;

	if (sched_hydra_smart_mode != 2)
		goto end;

	/* Get frequency of the first big core */
	if (!cpumask_empty(&hydra_big_cores)) {
		int cpu = cpumask_first(&hydra_big_cores);
		big_core_freq = cpufreq_quick_get(cpu);
	}

	if (big_core_freq > 0 && big_core_freq < sched_hydra_throttle_freq)
		is_throttled = true;

	cpumask_and(&allowed_mask, &hydra_big_cores, cpu_online_mask);

	mutex_lock(&hydra_mutex);
	for (i = 0; i < hydra_thread_count; i++) {
		struct hydra_thread_state *state = &hydra_threads[i];
		unsigned long util = 0;
		bool dead_or_reused = false;
		
		rcu_read_lock();
		t = find_task_by_pid_ns(state->tid, &init_pid_ns);
		if (t) {
			if (t->tgid == sched_hydra_pid) {
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

		if (dead_or_reused) {
			hydra_thread_count--;
			if (i < hydra_thread_count) {
				hydra_threads[i] = hydra_threads[hydra_thread_count];
				i--;
			}
			continue;
		}

		if (t) {
			if (is_throttled) {
				set_cpus_allowed_ptr(t, &state->original_mask);
			} else {
				if (util > sched_hydra_heavy_util) {
					if (!cpumask_empty(&allowed_mask))
						set_cpus_allowed_ptr(t, &allowed_mask);
				} else if (util < sched_hydra_light_util) {
					set_cpus_allowed_ptr(t, &state->original_mask);
				}
			}
			put_task_struct(t);
		}
	}
	mutex_unlock(&hydra_mutex);

end:
	if (hydra_smart_running)
		schedule_delayed_work(&hydra_smart_dwork, msecs_to_jiffies(500));
}

static void hydra_fork_work_fn(struct work_struct *work)
{
	int pid = READ_ONCE(sched_hydra_pid);
	if (pid > 0 && static_branch_likely(&sched_hydra_enable_key))
		hydra_optimize_threads(pid);
}

static void hydra_exit_work_fn(struct work_struct *work)
{
	if (atomic_xchg(&hydra_exit_pending, 0)) {
		hydra_revert_all_threads(true);
		WRITE_ONCE(sched_hydra_pid, 0);
		pr_info("hydra: auto-reverted on game exit\n");
	}
}

static void probe_sched_process_fork(void *ignore, struct task_struct *parent, struct task_struct *child)
{
	int pid = READ_ONCE(sched_hydra_pid);
	if (pid > 0 && parent->tgid == pid)
		schedule_work(&hydra_fork_work);
}

static void probe_sched_process_exit(void *ignore, struct task_struct *p)
{
	int pid = READ_ONCE(sched_hydra_pid);
	if (pid > 0 && p->tgid == pid && p->pid == pid) {
		atomic_set(&hydra_exit_pending, 1);
		schedule_work(&hydra_exit_work);
	}
}

static int sched_hydra_pid_handler(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	int old_pid = sched_hydra_pid;
	
	if (write && !capable(CAP_SYS_NICE))
		return -EPERM;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (sched_hydra_pid != old_pid) {
		if (old_pid > 0)
			hydra_revert_all_threads(true);

		if (sched_hydra_pid > 0 && static_branch_likely(&sched_hydra_enable_key))
			hydra_optimize_threads(sched_hydra_pid);
	}

	return 0;
}

static int sched_hydra_enable_handler(struct ctl_table *table, int write,
				      void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	int old = sched_hydra_enable;

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
				hydra_revert_all_threads(true);
		}
	}

	return 0;
}

static int sched_hydra_nice_handler(struct ctl_table *table, int write,
				    void __user *buffer, size_t *lenp, loff_t *ppos)
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

	if (sched_hydra_nice != old_nice && sched_hydra_pid > 0 && static_branch_likely(&sched_hydra_enable_key))
		hydra_optimize_threads(sched_hydra_pid);

	return 0;
}

static int sched_hydra_smart_mode_handler(struct ctl_table *table, int write,
					void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	int old_mode = sched_hydra_smart_mode;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (sched_hydra_smart_mode < 0)
		sched_hydra_smart_mode = 0;
	else if (sched_hydra_smart_mode > 2)
		sched_hydra_smart_mode = 2;

	if (sched_hydra_smart_mode != old_mode) {
		if (sched_hydra_smart_mode == 0) {
			stop_smart_worker();
			hydra_revert_cpumask_only();
		} else if (sched_hydra_smart_mode == 1) {
			stop_smart_worker();
			if (sched_hydra_pid > 0 && static_branch_likely(&sched_hydra_enable_key))
				hydra_optimize_threads(sched_hydra_pid);
		} else if (sched_hydra_smart_mode == 2) {
			if (sched_hydra_pid > 0 && static_branch_likely(&sched_hydra_enable_key)) {
				hydra_optimize_threads(sched_hydra_pid);
				start_smart_worker();
			}
		}
	}

	return 0;
}

static int sched_hydra_simple_int_handler(struct ctl_table *table, int write,
					void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_dointvec(table, write, buffer, lenp, ppos);
}

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
		.proc_handler	= sched_hydra_simple_int_handler,
	},
	{
		.procname	= "hydra_light_util",
		.data		= &sched_hydra_light_util,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sched_hydra_simple_int_handler,
	},
	{ }
};

static struct ctl_table_header *hydra_sysctl_header;

static int __init sched_init_hydra(void)
{
	INIT_WORK(&hydra_fork_work, hydra_fork_work_fn);
	INIT_WORK(&hydra_exit_work, hydra_exit_work_fn);
	INIT_DELAYED_WORK(&hydra_smart_dwork, hydra_smart_work_fn);

	register_trace_sched_process_fork(probe_sched_process_fork, NULL);
	register_trace_sched_process_exit(probe_sched_process_exit, NULL);

	hydra_sysctl_header = register_sysctl("kernel", hydra_sysctls);
	pr_info("hydra: " SCHED_HYDRA_PROGNAME " v" SCHED_HYDRA_VERSION " initialized.\n");
	return 0;
}
late_initcall(sched_init_hydra);

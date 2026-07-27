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

int __read_mostly sched_hydra_enable = 1;
DEFINE_STATIC_KEY_TRUE(sched_hydra_enable_key);

int sched_hydra_pid = 0;

static void hydra_optimize_threads(pid_t pid)
{
	struct task_struct *p, *t;
	cpumask_t big_cores;
	
	if (pid <= 0)
		return;

	cpumask_clear(&big_cores);
	cpumask_set_cpu(4, &big_cores);
	cpumask_set_cpu(5, &big_cores);
	cpumask_set_cpu(6, &big_cores);
	cpumask_set_cpu(7, &big_cores);
	cpumask_and(&big_cores, &big_cores, cpu_online_mask);

	if (cpumask_empty(&big_cores))
		return;

	rcu_read_lock();
	p = find_task_by_vpid(pid);
	if (!p) {
		rcu_read_unlock();
		return;
	}
	
	for_each_thread(p, t) {
		if (strnstr(t->comm, "RenderThread", TASK_COMM_LEN) ||
		    strnstr(t->comm, "UnityMain", TASK_COMM_LEN) ||
		    strnstr(t->comm, "UnityGfx", TASK_COMM_LEN) ||
		    strnstr(t->comm, "TaskGraph", TASK_COMM_LEN) ||
		    strnstr(t->comm, "GameThread", TASK_COMM_LEN) ||
		    strnstr(t->comm, "adreno", TASK_COMM_LEN) ||
		    strnstr(t->comm, "glthread", TASK_COMM_LEN) ||
		    strnstr(t->comm, "kgsl", TASK_COMM_LEN)) {
			
			set_user_nice(t, -10);
			set_cpus_allowed_ptr(t, &big_cores);
			pr_debug("hydra: optimized thread %d (%s)\n", t->pid, t->comm);
		}
	}
	rcu_read_unlock();
	pr_info("hydra: optimized game pid %d\n", pid);
}

static int sched_hydra_pid_handler(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (static_branch_likely(&sched_hydra_enable_key)) {
		if (sched_hydra_pid > 0)
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
		if (sched_hydra_enable)
			static_branch_enable(&sched_hydra_enable_key);
		else
			static_branch_disable(&sched_hydra_enable_key);
	}

	return 0;
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
	{ }
};

static struct ctl_table_header *hydra_sysctl_header;

static int __init sched_init_hydra(void)
{
	hydra_sysctl_header = register_sysctl("kernel", hydra_sysctls);
	pr_info("hydra: " SCHED_HYDRA_PROGNAME " v" SCHED_HYDRA_VERSION " initialized.\n");
	return 0;
}
late_initcall(sched_init_hydra);

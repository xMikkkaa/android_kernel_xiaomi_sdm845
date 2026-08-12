/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_HYDRA_H
#define _LINUX_SCHED_HYDRA_H

#include <linux/sched.h>
#include <linux/jump_label.h>
#include <linux/cpumask.h>
#include <linux/version.h>

#define SCHED_HYDRA_AUTHOR   "xMikkkaa"
#define SCHED_HYDRA_PROGNAME "HYDRA Game Thread Optimizer"
#define SCHED_HYDRA_VERSION  "0.9"

#define HYDRA_MAX_CLUSTERS 4

#define HYDRA_MAX_TRACKED_THREADS 64

/*
 * Compatibility macro for cpumask field rename in kernel 5.3+
 * (task_struct.cpus_allowed -> task_struct.cpus_mask)
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
#define hydra_cpus_allowed(t) ((t)->cpus_mask)
#else
#define hydra_cpus_allowed(t) ((t)->cpus_allowed)
#endif

/* Static Thread Pattern Matching */
static const char *hydra_comm_patterns[] = {
	"RenderThread",
	"UnityMain",
	"UnityGfx",
	"TaskGraph",
	"GameThread",
	"adreno",
	"glthread",
	"kgsl",
	"ANGLE",
	"FrameWorker"
};

struct hydra_thread_state {
	pid_t tid;
	u64 start_time;
	int original_nice;
	cpumask_t original_mask;
};

extern int __read_mostly sched_hydra_enable;
DECLARE_STATIC_KEY_TRUE(sched_hydra_enable_key);

extern int sched_hydra_pid;
extern int sched_hydra_nice;
extern int sched_hydra_smart_mode;
extern int sched_hydra_throttle_freq;
extern int sched_hydra_heavy_util;
extern int sched_hydra_light_util;

#endif /* _LINUX_SCHED_HYDRA_H */

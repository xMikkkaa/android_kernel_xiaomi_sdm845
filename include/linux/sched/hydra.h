#ifndef _KERNEL_SCHED_HYDRA_H
#define _KERNEL_SCHED_HYDRA_H

#include <linux/sched.h>
#include <linux/jump_label.h>
#include <linux/cpumask.h>

#define SCHED_HYDRA_AUTHOR   "xMikkkaa"
#define SCHED_HYDRA_PROGNAME "HYDRA Game Thread Optimizer"
#define SCHED_HYDRA_VERSION  "1.0"

#define HYDRA_MAX_TRACKED_THREADS 64

struct hydra_thread_state {
	pid_t tid;
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

#endif /* _KERNEL_SCHED_HYDRA_H */

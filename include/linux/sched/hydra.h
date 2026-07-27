#ifndef _KERNEL_SCHED_HYDRA_H
#define _KERNEL_SCHED_HYDRA_H

#include <linux/sched.h>
#include <linux/jump_label.h>

#define SCHED_HYDRA_AUTHOR   "xMikkkaa"
#define SCHED_HYDRA_PROGNAME "HYDRA Game Thread Optimizer"
#define SCHED_HYDRA_VERSION  "1.0"

extern int __read_mostly sched_hydra_enable;
DECLARE_STATIC_KEY_TRUE(sched_hydra_enable_key);

extern int sched_hydra_pid;

#endif /* _KERNEL_SCHED_HYDRA_H */

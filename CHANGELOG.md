# Chimera-V4 Changelog

## [V4] - 2026-08-16

### Memory & Performance
- Rewrite ION subsystem: RCU-accelerated handle/rbtree lookups, DMA sg_table caching, async workqueue concurrency limits
- Backport vmalloc improvements for faster large allocations
- IOMMU rewrite for clarity and performance
- Set ZRAM default compression to LZ4
- Optimized Console FrameBuffer for up to 70% performance increase
- Ashmem: fix SIGBUS crash on mmap traversal, convert range macros to inlines

### Scheduler & CPU
- BORE Scheduler: introduced at v5.1.0 then upgraded to v6.6.3
- SCHED_HYDRA: High-Yield Dynamic Render Affinity (v0.8 & v0.9 feature updates)
- Reflex cpufreq governor v0.3.1 with exponential frequency decay (set as default)
- WALT governor backported from linux 5.10
- NAP (Neural Adaptive Predictor) v0.5.0 cpuidle governor
- schedutil: exponential freq selection, enforce realtime priority, drop tracing
- Aggressive WALT upmigration tuning, disable LB_BIAS and EAS_USE_NEED_IDLE
- Optimize __calc_delta with fls bitwise shift
- Default I/O scheduler changed to maple

### Security & Root Hiding
- SUSFS 2.1.0+ backport, bumped to SUSFS 2.2.0 with mount source spoofing
- Hide `lineage` and `jit-zygote-cache` in `/proc/*/maps` and `map_files`
- SELinux: spoof kernel status page in selinuxfs
- Stop exposing kernel addresses via `/proc/kallsyms` and `/proc/module`

### Networking & eBPF
- Full eBPF/BPF JIT backport for ARM64 (standalone build without module support)
- TCP BBR v2 → v3 congestion control backports (inflight tracking, ECN mark handling, additive bw probing)
- RACK loss detection with reordering timer, 1ms TCP timestamp precision
- TCP TSQ optimizations: reduce atomics in tcp_wfree, shortcut in tcp_small_queue_check
- DCTCP ECN ACK handling refactor
- WireGuard: fix ram_pages type, switch optimization level from -O3 to -O2
- IPv4/IPv6: add second dif to inet socket lookups, dst_confirm_neigh support

### Drivers & Hardware
- gpucc-sdm845: undervolt GPU frequencies
- sdm845-v2: update GPU power levels and clock frequency table
- qpnp-smb2: add sysfs interface to bypass charging
- Bluetooth: fix double free in hci_conn_cleanup
- Audio: optimize wcd-mbhc-v2 codec, add non-WCD934X build support
- UFS: add health info, erase count, host writes to sysfs and kernel log
- Block Google Camera from running in the background

### Binder & IPC
- Checkout binder to android12-5.4 with compatibility fixes for 4.9
- Frozen notification support for binder nodes (including dead nodes)
- Fix OOB, UAF, and memory leaks in binder_add_freeze_work and binder_init
- binderfs: add support for feature files

### Build & Config
- Makefile: CPU-specific optimizations for SDM845
- kbuild: add config for optimised inlining, enable DCE (Dead Code Elimination)
- LZ4: import v1.10.0 from upstream with ARM64 v8 ASM decompression acceleration
- EROFS filesystem support (ported from 5.10)
- Only build DTB for current xiaomi device
- Config: enable SCHED_HYDRA, EROFS, Match Owner netfilter, CLS_BPF, BPF_JIT, Kallsyms
- RCU: consolidated reader checking, speed up tasks callbacks

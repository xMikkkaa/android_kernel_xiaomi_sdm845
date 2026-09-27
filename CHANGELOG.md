# Chimera-V5 Changelog

## [V5] - 2026-09-27

### Scheduler & CPU
- EEVDF (Earliest Eligible Virtual Deadline First) scheduler backport with BORE 6.6.3 integration
- EEVDF: disable PLACE_LAG and RUN_TO_PARITY by default, initialize vslice in place_entity()
- Uclamp backport: full utilization clamping framework with cpuset tunables, latency-sensitive flag, RT default boost sysctl, cgroup controller, bucket refcounting, and static key fast path
- Uclamp integration: WALT colocation awareness, boosted task placement optimization, schedutil frequency reflection
- SCHED_HYDRA: match UE4-era game threads, debounce fork bursts, hoist pattern parsing, skip redundant affinity sets
- WALT: aggressive migration defaults for responsiveness, raise big-cluster rtg boost floor for gaming
- Reflex cpufreq governor consolidated into schedutil: hispeed floor tunables, log-domain decay helpers, busy% blend into freq selection
- Default governor changed to performance
- Disable sched autogroup, revert thermal-engine forced thermal policy

### Memory & Filesystem
- LE9EC: Low-memory protection with sysctl knobs for protecting the working set
- mm: fix unmap_mapping_range high bits shift, reject unaligned remap_pfn_range, VM_PAT COW handling, PGDAT_RECLAIM_LOCKED barrier
- memcg: fix soft lockup in OOM, UAF in drain_all_stock(), user-triggerable oops in event_control, always cond_resched() after fn()
- vmscan: account for free pages to prevent infinite loop in throttle_direct_reclaim()
- ext4: fix slab-use-after-free in split_extent_at, UAF in ext_insert_extent, double brelse, OOB dotdot read, off-by-one in do_split, inline len overflow, INLINE_DATA_FL without xattr
- fuse: unsigned type for getxattr/listxattr truncation, initialize beyond-EOF page contents
- fat: fix uninitialized variable and nostale filehandle field
- fs/buffer: fix use-after-free in bh_read() helper
- block: fix integer overflow in segment size merge, kmem_cache name collision, signed overflow in Amiga partition, bogus mac partition table
- mount: uniform permission checks for all propagation changes

### Security & Root Hiding
- SUSFS bumped to 2.3.0
- SUSFS: refactor SUS_MOUNT/SUS_KSTAT/OPEN_REDIRECT spoofing logic, fix mount minor dev gap detection, fix kstat spoofing in vfs_statfs and statfs_by_dentry
- SUSFS: fix OPEN_REDIRECT UAF and memory leak/deadlock, optimize hide_sus_mnts, temp fix for zygote_next exploit, narrow __lookup_mnt hook
- SUSFS: mark no-root app process with TIF_PROC_NO_SU flag, remove strncpy(), fix userspace feature parsing, fix ksu_driver_su install order
- SUSFS: add KSU_SUSFS_AUTO_MOUNT_SOURCE_SPOOF config option
- Revert proc maps/map_files hiding for lineage and jit-zygote-cache
- SELinux: modify RTM_GETNEIGH{TBL} (backport from ANDROID)
- KernelSU: fix ksu_selinux_hide patch for newer versions

### Networking & WireGuard
- WireGuard: updated compat layer — handle backported rng/blake2s, CFI-safe ptr_ring cleanup, metadata_dst check, IPv6-disabled socket fixes
- TCP: quiet retransmit-timer empty-queue race to rate-limited log

### Drivers & Hardware
- USB fast charge mode driver
- USB: enable/disable USB data attributes, handle charging behavior when data is disabled
- Thermal: fix bcl_peripheral battery percentage read
- nl80211: add WPA3 SAE authentication definition
- Power supply: silence log spam
- DTS sdm845: disable coresight, suppress verbose boot output, disable broken IRQ monitoring, disable expedited RCU grace periods after init, move to second-stage init, disable memcg kernel/socket accounting, increase CMA from 32M to 128M, enable DSI phy idle mode, alter DSI supply disable load, enable PM QoS for rotator
- DTS beryllium: drop NFC support
- GPU OC: add stepped OC tables for 805/820/835/844 via per-variant patch files
- Revert thermal-engine forced thermal policy on cpu_cooling

### Binder & IPC
- Binder: fix max_thread type inconsistency, check offset alignment in binder_get_object(), fix UAF caused by offsets overwrite

### Build & Toolchain
- GCC LTO infrastructure: Link Time Optimization support with __noreorder initcalls
- Clang PGO infrastructure adapted for LLVM 23, with release.sh for profile-guided builds
- GCC 14.2 build script (gcompile.sh) with ccache support
- LD_DEAD_CODE_DATA_ELIMINATION: enable selection on arm64, fix vmlinux.lds.h
- kbuild: gate Clang-only flags behind cc-name check, dedup CPU tuning blocks, drop invalid +sve, enable crypto on big.LITTLE tuning, disable stack conservation for GCC
- vDSO32: LLVM integrated AS support, hardcode toolchain target, suppress mrproper errors, remove jump label config, install from vdso_install, exclude from gcov
- gcov: add GCC 14 support
- Disable LTO in chimera config, set BBR as default congestion control, enable workqueue power-efficient, configure le9ec, enable uclamp alongside schedtune
- Build scripts: fstab variants via patch files, rebrand CI builds to Chimera-CI
- Documentation: convert README and module-signing to ReST markup

### Code Quality
- Fix ~50 compiler warnings: misleading indentation, dead NULL checks, duplicated const, unbraced error paths, printf format mismatches, unused variables, prototype mismatches, gnu89 declaration ordering
- Fix false stringop-overread warnings in WireGuard/zinc
- dynamic_debug and mm/extable: compare table bounds as addresses

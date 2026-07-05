# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the NAP cpuidle governor
#

obj-$(CONFIG_CPU_IDLE_GOV_NAP) += cpuidle_gov_nap.o

cpuidle_gov_nap-y := nap.o nap_fpu.o nap_nn_c.o

ccflags-y += -O2

# Remove general-regs-only so the compiler can use FP/SIMD registers for float math
CFLAGS_REMOVE_nap_fpu.o  += -mgeneral-regs-only
CFLAGS_REMOVE_nap_nn_c.o += -mgeneral-regs-only

CFLAGS_nap_fpu.o  += -ffreestanding -O2
CFLAGS_nap_nn_c.o += -ffreestanding -O2

#!/usr/bin/env bash
# ============================================================================
#  Chimera Kernel Build Script for Xiaomi Poco F1 (beryllium)
#  Kernel: Linux 4.9.337 (arm64 / SDM845)
#  Toolchain: Neutron Clang 23.0.0git
#  Author: xMikkkaa
# ============================================================================

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
#  Color definitions
# ─────────────────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# ─────────────────────────────────────────────────────────────────────────────
#  Path configurations
# ─────────────────────────────────────────────────────────────────────────────
# Kernel source root (directory where this script lives)
KERNEL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Neutron Clang toolchain
CLANG_DIR="${KERNEL_DIR}/neutron-clang"
CLANG_BIN="${CLANG_DIR}/bin"

# AnyKernel3 directory (for flashable zip packaging)
ANYKERNEL_DIR="${KERNEL_DIR}/tools/AnyKernel3"

# Output directory (out-of-tree build to keep source clean)
OUT_DIR="${KERNEL_DIR}/out"

# ─────────────────────────────────────────────────────────────────────────────
#  Build configurations
# ─────────────────────────────────────────────────────────────────────────────
# Defconfig
DEFCONFIG="chimera_defconfig"

# Architecture
ARCH="arm64"

# Target kernel image (Image.gz-dtb as configured in defconfig)
KERNEL_IMAGE="Image.gz-dtb"

# Number of parallel jobs (all available cores + 2 for I/O overlap)
JOBS="$(nproc --all)"

# Kernel name from defconfig LOCALVERSION
OC_VAL="805"
KERNEL_NAME="Chimera-CI"

# Build variant (default or nse)
VARIANT="default"

# Disable Audio flag
DISABLE_AUDIO="false"

# Zip output directory
ZIP_DIR="${KERNEL_DIR}/out/zip"

# ─────────────────────────────────────────────────────────────────────────────
#  Clang/LLVM tool variables
# ─────────────────────────────────────────────────────────────────────────────
export PATH="${CLANG_BIN}:${PATH}"
export ARCH="${ARCH}"
export SUBARCH="${ARCH}"
export KBUILD_BUILD_USER="xMikkkaa"
export KBUILD_BUILD_HOST="Sunny"

# ─────────────────────────────────────────────────────────────────────────────
#  Make arguments
# ─────────────────────────────────────────────────────────────────────────────
MAKE_ARGS=(
    O="${OUT_DIR}"
    ARCH="${ARCH}"
    SUBARCH="${ARCH}"
    LLVM=1
    LLVM_IAS=1
    CC="clang"
    LD="ld.lld"
    AR="llvm-ar"
    NM="llvm-nm"
    OBJCOPY="llvm-objcopy"
    OBJDUMP="llvm-objdump"
    STRIP="llvm-strip"
    OBJSIZE="llvm-size"
    READELF="llvm-readelf"
    CROSS_COMPILE="aarch64-linux-gnu-"
    CROSS_COMPILE_ARM32="arm-linux-gnueabi-"
    CLANG_TRIPLE="aarch64-linux-gnu-"
    LOCALVERSION="-${KERNEL_NAME}"
)

# ─────────────────────────────────────────────────────────────────────────────
#  Helper functions
# ─────────────────────────────────────────────────────────────────────────────
log_info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $*"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()    { echo -e "\n${CYAN}${BOLD}═══════════════════════════════════════════${NC}"; \
                echo -e "${CYAN}${BOLD}  $*${NC}"; \
                echo -e "${CYAN}${BOLD}═══════════════════════════════════════════${NC}\n"; }

timer_start() { BUILD_START=$(date +"%s"); }
timer_end() {
    local BUILD_END=$(date +"%s")
    local DIFF=$((BUILD_END - BUILD_START))
    echo -e "\n${GREEN}${BOLD}⏱  Time elapsed: $((DIFF / 60)) minute(s) and $((DIFF % 60)) second(s)${NC}\n"
}

run_release_builds() {
    log_step "Running release build sequence"

    local -a release_commands=(
        "--dirty"
        "--820 --dirty"
        "--835 --dirty"
        "--844 --dirty"
        "--nse --dirty"
        "--nse --820 --dirty"
        "--nse --835 --dirty"
        "--nse --844 --dirty"
        "--dynamic --dirty"
        "--dynamic --820 --dirty"
        "--dynamic --835 --dirty"
        "--dynamic --844 --dirty"
    )

    for release_command in "${release_commands[@]}"; do
        local -a release_args=()
        read -r -a release_args <<< "${release_command}"

        log_info "Running: ${KERNEL_DIR}/compile.sh ${release_args[*]}"
        "${KERNEL_DIR}/compile.sh" "${release_args[@]}"
    done

    log_success "Release build sequence completed"
}

# ─────────────────────────────────────────────────────────────────────────────
#  Pre-flight checks
# ─────────────────────────────────────────────────────────────────────────────
preflight_check() {
    log_step "Pre-flight Checks"

    if [ ! -x "${CLANG_BIN}/clang" ]; then
        log_error "Clang not found at ${CLANG_BIN}/clang"
        exit 1
    fi

    local CLANG_VERSION
    CLANG_VERSION=$("${CLANG_BIN}/clang" --version | head -1)
    log_info "Clang: ${CLANG_VERSION}"

    if [ ! -e "${CLANG_BIN}/ld.lld" ]; then
        log_error "ld.lld not found at ${CLANG_BIN}/ld.lld"
        exit 1
    fi
    log_info "Linker: ld.lld (bundled with Neutron Clang)"

    if [ ! -f "${KERNEL_DIR}/arch/${ARCH}/configs/${DEFCONFIG}" ]; then
        log_error "Defconfig not found: arch/${ARCH}/configs/${DEFCONFIG}"
        exit 1
    fi
    log_info "Defconfig: ${DEFCONFIG}"

    if [ ! -d "${ANYKERNEL_DIR}" ]; then
        log_error "AnyKernel3 not found at ${ANYKERNEL_DIR}"
        exit 1
    fi
    if [ ! -f "${ANYKERNEL_DIR}/anykernel.sh" ]; then
        log_error "anykernel.sh not found in AnyKernel3 directory"
        exit 1
    fi
    log_info "AnyKernel3: ${ANYKERNEL_DIR}"

    log_info "Kernel: Linux 4.9.337"
    log_info "Device: Xiaomi Poco F1 (beryllium / SDM845)"
    log_info "Target: ${KERNEL_IMAGE}"
    log_info "Jobs: ${JOBS}"

    log_success "All pre-flight checks passed!"
}

# (Modular defconfig removed for legacy)

# ─────────────────────────────────────────────────────────────────────────────
#  Disable audio configurations in generated defconfig
# ─────────────────────────────────────────────────────────────────────────────
disable_audio_configs() {
    log_step "Disabling Audio Configurations..."
    local TARGET_CONFIG="${KERNEL_DIR}/arch/${ARCH}/configs/${DEFCONFIG}"
    
    # Back up original defconfig before modifying
    cp "${TARGET_CONFIG}" "${TARGET_CONFIG}.bak"
    
    sed -i 's/CONFIG_SND_SOC_SDM845=y/# CONFIG_SND_SOC_SDM845 is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_MACHINE_SDM845=y/# CONFIG_SND_SOC_MACHINE_SDM845 is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_WCD934X=y/# CONFIG_SND_SOC_WCD934X is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_WCD934X_DSD=y/# CONFIG_SND_SOC_WCD934X_DSD is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_WCD934X_MBHC=y/# CONFIG_SND_SOC_WCD934X_MBHC is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_WCD9XXX_V2=y/# CONFIG_SND_SOC_WCD9XXX_V2 is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_WCD_MBHC=y/# CONFIG_SND_SOC_WCD_MBHC is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_WCD_MBHC_ADC=y/# CONFIG_SND_SOC_WCD_MBHC_ADC is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_WCD_SPI=y/# CONFIG_SND_SOC_WCD_SPI is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SND_SOC_WSA881X=y/# CONFIG_SND_SOC_WSA881X is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_SOUNDWIRE_WCD_CTRL=y/# CONFIG_SOUNDWIRE_WCD_CTRL is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_WCD9XXX_CODEC_CORE=y/# CONFIG_WCD9XXX_CODEC_CORE is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_WCD_DSP_GLINK=y/# CONFIG_WCD_DSP_GLINK is not set/' "${TARGET_CONFIG}"
    sed -i 's/CONFIG_WCD_SPI_AC=y/# CONFIG_WCD_SPI_AC is not set/' "${TARGET_CONFIG}"
    
    log_success "Audio configurations disabled in ${DEFCONFIG}"
}

# ─────────────────────────────────────────────────────────────────────────────
#  Step 1: Clean build (optional)
# ─────────────────────────────────────────────────────────────────────────────
clean_build() {
    log_step "Cleaning Build Directory"

    if [ -d "${OUT_DIR}" ]; then
        rm -rf "${OUT_DIR}"
        log_info "Removed existing output directory: ${OUT_DIR}"
    fi

    mkdir -p "${OUT_DIR}"
    log_success "Clean build directory created"
}

# ─────────────────────────────────────────────────────────────────────────────
#  Step 2: Generate defconfig
# ─────────────────────────────────────────────────────────────────────────────
generate_defconfig() {
    log_step "Generating Defconfig: ${DEFCONFIG}"

    make -C "${KERNEL_DIR}" "${MAKE_ARGS[@]}" "${DEFCONFIG}" -j"${JOBS}"

    if [ ! -f "${OUT_DIR}/.config" ]; then
        log_error "Failed to generate .config from ${DEFCONFIG}"
        exit 1
    fi

    log_success "Defconfig generated successfully"
}

# ─────────────────────────────────────────────────────────────────────────────
#  Step 3: Build kernel
# ─────────────────────────────────────────────────────────────────────────────
build_kernel() {
    log_step "Building Kernel (${KERNEL_IMAGE})"
    
    if [ -f "${OUT_DIR}/.version" ]; then
        rm "${OUT_DIR}/.version"
    fi

    timer_start

    make -C "${KERNEL_DIR}" "${MAKE_ARGS[@]}" -j"${JOBS}" "${KERNEL_IMAGE}" 2>&1 | tee "${OUT_DIR}/build.log"

    if [ ! -f "${OUT_DIR}/arch/${ARCH}/boot/${KERNEL_IMAGE}" ]; then
        log_error "Kernel image not found: ${OUT_DIR}/arch/${ARCH}/boot/${KERNEL_IMAGE}"
        log_error "Build failed! Check ${OUT_DIR}/build.log for details."
        exit 1
    fi

    timer_end

    local IMG_SIZE
    IMG_SIZE=$(du -h "${OUT_DIR}/arch/${ARCH}/boot/${KERNEL_IMAGE}" | awk '{print $1}')
    log_success "Kernel built successfully!"
    log_info "Image: ${OUT_DIR}/arch/${ARCH}/boot/${KERNEL_IMAGE} (${IMG_SIZE})"
}

# ─────────────────────────────────────────────────────────────────────────────
#  Step 4: Package with AnyKernel3
# ─────────────────────────────────────────────────────────────────────────────
package_zip() {
    log_step "Packaging Flashable Zip with AnyKernel3"

    local STAGING_DIR="${OUT_DIR}/anykernel_staging"
    rm -rf "${STAGING_DIR}"
    mkdir -p "${STAGING_DIR}"

    cp -r "${ANYKERNEL_DIR}"/* "${STAGING_DIR}"/
    cp "${OUT_DIR}/arch/${ARCH}/boot/${KERNEL_IMAGE}" "${STAGING_DIR}/"
    log_info "Copied ${KERNEL_IMAGE} to AnyKernel3 staging"

    local ZIP_NAME="${KERNEL_NAME}.zip"

    mkdir -p "${ZIP_DIR}"

    cd "${STAGING_DIR}"
    zip -r9 "${ZIP_DIR}/${ZIP_NAME}" . \
        -x '*.git*' \
        -x '*README*' \
        -x '*LICENSE*' \
        -x '*.md'
    cd "${KERNEL_DIR}"

    rm -rf "${STAGING_DIR}"

    if [ ! -f "${ZIP_DIR}/${ZIP_NAME}" ]; then
        log_error "Failed to create flashable zip"
        exit 1
    fi

    local ZIP_SIZE
    ZIP_SIZE=$(du -h "${ZIP_DIR}/${ZIP_NAME}" | awk '{print $1}')

    log_success "Flashable zip created!"
    log_info "File: ${ZIP_DIR}/${ZIP_NAME}"
    log_info "Size: ${ZIP_SIZE}"

    echo -e "\n${GREEN}${BOLD}╔═══════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}${BOLD}║                  BUILD COMPLETE!                  ║${NC}"
    echo -e "${GREEN}${BOLD}╠═══════════════════════════════════════════════════╣${NC}"
    echo -e "${GREEN}${BOLD}║${NC}  Kernel : ${KERNEL_NAME}$(printf '%*s' $((27 - ${#KERNEL_NAME})) '')${GREEN}${BOLD}║${NC}"
    echo -e "${GREEN}${BOLD}║${NC}  Device : beryllium (Poco F1)              ${GREEN}${BOLD}║${NC}"
    echo -e "${GREEN}${BOLD}║${NC}  Zip    : ${ZIP_NAME}$(printf '%*s' $((27 - ${#ZIP_NAME} + 12)) '')${GREEN}${BOLD}║${NC}"
    echo -e "${GREEN}${BOLD}║${NC}  Size   : ${ZIP_SIZE}$(printf '%*s' $((34 - ${#ZIP_SIZE})) '')${GREEN}${BOLD}║${NC}"
    echo -e "${GREEN}${BOLD}╠═══════════════════════════════════════════════════╣${NC}"
    echo -e "${GREEN}${BOLD}║${NC}  Flash via TWRP/custom recovery:               ${GREEN}${BOLD}║${NC}"
    echo -e "${GREEN}${BOLD}║${NC}  ${CYAN}adb sideload ${ZIP_NAME}${NC}$(printf '%*s' $((1)) '')${GREEN}${BOLD}║${NC}"
    echo -e "${GREEN}${BOLD}╚═══════════════════════════════════════════════════╝${NC}"
}

# ─────────────────────────────────────────────────────────────────────────────
#  Step 5: Regenerate defconfig (optional, for development)
# ─────────────────────────────────────────────────────────────────────────────
regen_defconfig() {
    log_step "Regenerating Defconfig"

    make -C "${KERNEL_DIR}" "${MAKE_ARGS[@]}" savedefconfig -j"${JOBS}"

    if [ -f "${OUT_DIR}/defconfig" ]; then
        cp "${OUT_DIR}/defconfig" "${KERNEL_DIR}/arch/${ARCH}/configs/${DEFCONFIG}"
        log_success "Defconfig regenerated and saved to arch/${ARCH}/configs/${DEFCONFIG}"
    else
        log_error "Failed to regenerate defconfig"
        exit 1
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  Usage / Help
# ─────────────────────────────────────────────────────────────────────────────
show_help() {
    echo -e "${CYAN}${BOLD}"
    echo "  ╔═══════════════════════════════════════════════════╗"
    echo "  ║     Chimera Kernel Build Script - Beryllium       ║"
    echo "  ╚═══════════════════════════════════════════════════╝"
    echo -e "${NC}"
    echo "  Usage: $0 [OPTION]"
    echo ""
    echo "  Options:"
    echo "    (no args)    Full build: clean → defconfig → build → zip"
    echo "    --dirty      Build without cleaning (incremental build)"
    echo "    --release    Run the predefined release build sequence"
    echo "    --clean      Only clean the build directory"
    echo "    --defconfig  Only generate the defconfig"
    echo "    --build      Only build the kernel (assumes defconfig exists)"
    echo "    --zip        Only package the zip (assumes kernel is built)"
    echo "    --regen      Regenerate and save defconfig"
    echo "    --help       Show this help message"
    echo ""
    echo "  GPU Overclock Options:"
    echo "    --805        805 MHz GPU Frequency (Default)"
    echo "    --820        820 MHz GPU Frequency"
    echo "    --835        835 MHz GPU Frequency"
    echo "    --844        844 MHz GPU Frequency"
    echo ""
    echo "  Variant Options:"
    echo "    --nse        Non-System_Ext Variant"
    echo "    --dynamic    Dynamic Partition Variant (No fstab injection)"
    echo ""
    echo "  Audio Options:"
    echo "    --no-audio   Disable Audio Variant"
    echo ""
}

# ─────────────────────────────────────────────────────────────────────────────
#  Apply GPU Overclock
# ─────────────────────────────────────────────────────────────────────────────
apply_gpu_oc() {
    log_step "Applying GPU Overclock: ${OC_VAL} MHz"
    
    sed -i -E "s/8(05|20|35|44)000000/${OC_VAL}000000/g" "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-v2.dtsi"
    sed -i -E "s/8(05|20|35|44)000000/${OC_VAL}000000/g" "${KERNEL_DIR}/drivers/clk/qcom/gpucc-sdm845.c"
    
    log_success "GPU frequency set to ${OC_VAL} MHz in DT and Clock Driver"
}

restore_gpu_oc() {
    if [ "${OC_VAL}" != "805" ]; then
        log_step "Restoring original GPU frequency configuration..."
        sed -i -E "s/${OC_VAL}000000/805000000/g" "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-v2.dtsi"
        sed -i -E "s/${OC_VAL}000000/805000000/g" "${KERNEL_DIR}/drivers/clk/qcom/gpucc-sdm845.c"
        log_success "Original files restored"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  Apply fstab variant
# ─────────────────────────────────────────────────────────────────────────────
apply_fstab_variant() {
    if [ "${VARIANT}" = "dynamic" ]; then
        log_info "Dynamic partition variant: skipping fstab injection..."
        return
    fi

    log_step "Applying fstab configuration..."
    cp "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-xiaomi-common.dtsi" "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-xiaomi-common.dtsi.bak"
    
    if [ "${VARIANT}" = "nse" ]; then
        log_info "Applying NSE (Non-System_Ext) fstab..."
        cat << 'EOF' >> "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-xiaomi-common.dtsi"

/* NSE Fstab appended by compile.sh */
&firmware {
	android {
		fstab {
			compatible = "android,fstab";
			system {
				compatible = "android,system";
				dev = "/dev/block/platform/soc/1d84000.ufshc/by-name/system";
				type = "ext4";
				mnt_flags = "ro,barrier=1,discard";
				fsmgr_flags = "wait";
				status = "ok";
			};
			vendor {
				compatible = "android,vendor";
				dev = "/dev/block/platform/soc/1d84000.ufshc/by-name/vendor";
				type = "ext4";
				mnt_flags = "ro,barrier=1,discard";
				fsmgr_flags = "wait";
				status = "ok";
			};
		};
	};
};
EOF
        log_success "NSE fstab applied to sdm845-xiaomi-common.dtsi"
    else
        log_info "Applying Default (System_Ext) fstab..."
        cat << 'EOF' >> "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-xiaomi-common.dtsi"

/* Default Fstab appended by compile.sh */
&firmware {
	android {
		fstab {
			compatible = "android,fstab";
			system {
				compatible = "android,system";
				dev = "/dev/block/platform/soc/1d84000.ufshc/by-name/system";
				type = "ext4";
				mnt_flags = "ro,barrier=1,discard";
				fsmgr_flags = "wait";
				status = "ok";
			};
			system_ext {
				compatible = "android,system_ext";
				dev = "/dev/block/platform/soc/1d84000.ufshc/by-name/cust";
				type = "ext4";
				mnt_flags = "ro,barrier=1,discard";
				fsmgr_flags = "wait";
				status = "ok";
			};
			vendor {
				compatible = "android,vendor";
				dev = "/dev/block/platform/soc/1d84000.ufshc/by-name/vendor";
				type = "ext4";
				mnt_flags = "ro,barrier=1,discard";
				fsmgr_flags = "wait";
				status = "ok";
			};
		};
	};
};
EOF
        log_success "Default fstab applied to sdm845-xiaomi-common.dtsi"
    fi
}

restore_fstab_variant() {
    if [ -f "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-xiaomi-common.dtsi.bak" ]; then
        log_step "Restoring original fstab configuration..."
        mv "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-xiaomi-common.dtsi.bak" "${KERNEL_DIR}/arch/arm64/boot/dts/qcom/sdm845-xiaomi-common.dtsi"
        log_success "Original sdm845-xiaomi-common.dtsi restored"
    fi
}

restore_audio_configs() {
    local TARGET_CONFIG="${KERNEL_DIR}/arch/${ARCH}/configs/${DEFCONFIG}"
    if [ -f "${TARGET_CONFIG}.bak" ]; then
        log_step "Restoring original audio configuration in defconfig..."
        mv "${TARGET_CONFIG}.bak" "${TARGET_CONFIG}"
        log_success "Original ${DEFCONFIG} restored"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  Main entry point
# ─────────────────────────────────────────────────────────────────────────────
main() {
    local ACTION="full"

    for arg in "$@"; do
        if [ "${arg}" = "--release" ]; then
            run_release_builds
            exit 0
        fi
    done

    for arg in "$@"; do
        case "${arg}" in
            --805)
                OC_VAL="805"
                ;;
            --820)
                OC_VAL="820"
                ;;
            --835)
                OC_VAL="835"
                ;;
            --844)
                OC_VAL="844"
                ;;
            --nse)
                VARIANT="nse"
                ;;
            --dynamic)
                VARIANT="dynamic"
                ;;

            --no-audio)
                DISABLE_AUDIO="true"
                ;;
            --help|-h|--clean|--defconfig|--build|--zip|--dirty|--regen)
                ACTION="${arg}"
                ;;
            *)
                log_error "Unknown option: ${arg}"
                show_help
                exit 1
                ;;
        esac
    done

    if [ "${VARIANT}" = "nse" ] && [ "${DISABLE_AUDIO}" = "true" ]; then
        KERNEL_NAME="${KERNEL_NAME}-NSE-Disable-Audio-OC${OC_VAL}"
    elif [ "${VARIANT}" = "nse" ]; then
        KERNEL_NAME="${KERNEL_NAME}-NSE-OC${OC_VAL}"
    elif [ "${VARIANT}" = "dynamic" ] && [ "${DISABLE_AUDIO}" = "true" ]; then
        KERNEL_NAME="${KERNEL_NAME}-Dynamic-Disable-Audio-OC${OC_VAL}"
    elif [ "${VARIANT}" = "dynamic" ]; then
        KERNEL_NAME="${KERNEL_NAME}-Dynamic-OC${OC_VAL}"
    elif [ "${DISABLE_AUDIO}" = "true" ]; then
        KERNEL_NAME="${KERNEL_NAME}-Disable-Audio-OC${OC_VAL}"
    else
        KERNEL_NAME="${KERNEL_NAME}-OC${OC_VAL}"
    fi

    if [ "${ACTION}" = "--help" ] || [ "${ACTION}" = "-h" ]; then
        show_help
        exit 0
    fi

    trap 'restore_gpu_oc; restore_fstab_variant; restore_audio_configs' EXIT

    apply_gpu_oc
    apply_fstab_variant
    
    if [ "${ACTION}" != "--clean" ]; then
        if [ "${DISABLE_AUDIO}" = "true" ]; then
            disable_audio_configs
        fi
    fi

    case "${ACTION}" in
        --clean)
            clean_build
            ;;
        --defconfig)
            preflight_check
            generate_defconfig
            ;;
        --build)
            preflight_check
            build_kernel
            ;;
        --zip)
            preflight_check
            package_zip
            ;;
        --dirty)
            preflight_check
            generate_defconfig
            build_kernel
            package_zip
            ;;
        --regen)
            preflight_check
            regen_defconfig
            ;;
        full)
            preflight_check
            clean_build
            generate_defconfig
            build_kernel
            package_zip
            ;;
    esac
}

# Run
main "$@"
#!/usr/bin/env bash
# ============================================================================
#  Chimera Kernel Build Script for Xiaomi Poco F1 (beryllium)
#  Kernel: Linux 4.9.337 (arm64 / SDM845)
#  Toolchain: Arm GNU GCC 14.2 (aarch64 + arm32 for vdso32).
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

TOOLCHAINS_DIR="${TOOLCHAINS_DIR:-${HOME}/xMik-Project/toolchains}"

TC_DIR=""
TC32_DIR=""

GCC64_DIR="${TOOLCHAINS_DIR}/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu"
GCC32_DIR="${TOOLCHAINS_DIR}/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-linux-gnueabihf"

TC_BIN=""
TC32_BIN=""
MAKE_ARGS=()

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
KERNEL_NAME="Chimera-V5-Rc2"

# Build variant (default or nse)
VARIANT="default"

# Disable Audio flag
DISABLE_AUDIO="false"

# Zip output directory
ZIP_DIR="${KERNEL_DIR}/out/zip"

# ─────────────────────────────────────────────────────────────────────────────
#  GCC tool variables
# ─────────────────────────────────────────────────────────────────────────────
setup_toolchain() {
    TC_BIN="${TC_DIR:-${GCC64_DIR}}/bin"
    TC32_BIN="${TC32_DIR:-${GCC32_DIR}}/bin"
    export PATH="${TC_BIN}:${TC32_BIN}:${PATH}"
    export ARCH="${ARCH}"
    export SUBARCH="${ARCH}"
    export KBUILD_BUILD_USER="xMikkkaa"
    export KBUILD_BUILD_HOST="Sunny"
}

# ─────────────────────────────────────────────────────────────────────────────
#  Make arguments
# ─────────────────────────────────────────────────────────────────────────────
setup_make_args() {
    MAKE_ARGS=(
        O="${OUT_DIR}"
        ARCH="${ARCH}"
        SUBARCH="${ARCH}"
        CC="ccache aarch64-none-linux-gnu-gcc"
        CROSS_COMPILE="aarch64-none-linux-gnu-"
        CCARM32="ccache arm-none-linux-gnueabihf-gcc"
        CROSS_COMPILE_ARM32="arm-none-linux-gnueabihf-"
        LOCALVERSION="-${KERNEL_NAME}"
    )
}

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

    if [ ! -x "${TC_BIN}/aarch64-none-linux-gnu-gcc" ]; then
        log_error "aarch64 GCC not found at ${TC_BIN}/aarch64-none-linux-gnu-gcc"
        log_error "Set TOOLCHAINS_DIR or pass --tc-dir=<path>"
        exit 1
    fi

    local GCC_VERSION
    GCC_VERSION=$("${TC_BIN}/aarch64-none-linux-gnu-gcc" --version | head -1)
    log_info "GCC: ${GCC_VERSION}"

    if [ ! -x "${TC_BIN}/aarch64-none-linux-gnu-ld" ]; then
        log_error "cross ld not found in ${TC_BIN}"
        exit 1
    fi
    log_info "Linker: aarch64-none-linux-gnu-ld (GNU BFD)"

    if [ ! -x "${TC32_BIN}/arm-none-linux-gnueabihf-gcc" ]; then
        log_error "arm32 GCC not found at ${TC32_BIN}/arm-none-linux-gnueabihf-gcc"
        log_error "Set --tc32-dir=<path> (needed for vdso32)"
        exit 1
    fi
    log_info "ARM32 GCC: present (vdso32)"

    if command -v ccache &>/dev/null; then
        log_info "ccache: $(ccache --version | head -1) -> enabled"
    else
        log_warn "ccache not found - builds will run without cache acceleration"
    fi

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
collect_warnings() {
    grep -iE "warning:" "${OUT_DIR}/build.log" > "${OUT_DIR}/warning.log" || true

    local WARN_COUNT
    WARN_COUNT=$(wc -l < "${OUT_DIR}/warning.log")
    log_info "Warnings: ${WARN_COUNT} (see ${OUT_DIR}/warning.log)"
}

build_kernel() {
    log_step "Building Kernel (${KERNEL_IMAGE})"
    
    if [ -f "${OUT_DIR}/.version" ]; then
        rm "${OUT_DIR}/.version"
    fi

    timer_start

    make -C "${KERNEL_DIR}" "${MAKE_ARGS[@]}" -j"${JOBS}" "${KERNEL_IMAGE}" 2>&1 | tee "${OUT_DIR}/build.log"

    collect_warnings

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
    printf '%s\n' "${BASE_KERNEL_NAME}" > "${OUT_DIR}/kernel_name"

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
    echo "    --tc-dir=DIR Override 64-bit GCC toolchain dir"
    echo "    --tc32-dir=DIR Override 32-bit GCC toolchain dir (vdso32)"
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

    local OC_PATCH="${KERNEL_DIR}/patches/gpu-oc/oc-${OC_VAL}.patch"
    if [ ! -f "${OC_PATCH}" ]; then
        log_error "OC patch not found: ${OC_PATCH}"
        exit 1
    fi
    patch -p1 -d "${KERNEL_DIR}" < "${OC_PATCH}"

    log_success "Stepped GPU table ${OC_VAL} MHz applied"
}

restore_gpu_oc() {
    log_step "Restoring stock GPU tables..."
    git -C "${KERNEL_DIR}" checkout -- \
        arch/arm64/boot/dts/qcom/sdm845-v2.dtsi \
        drivers/clk/qcom/gpucc-sdm845.c
    log_success "Stock GPU tables restored"
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

    local FSTAB_PATCH
    if [ "${VARIANT}" = "nse" ]; then
        log_info "Applying NSE (Non-System_Ext) fstab..."
        FSTAB_PATCH="${KERNEL_DIR}/patches/fstab/fstab-nse.patch"
    else
        log_info "Applying Default (System_Ext) fstab..."
        FSTAB_PATCH="${KERNEL_DIR}/patches/fstab/fstab-default.patch"
    fi

    if [ ! -f "${FSTAB_PATCH}" ]; then
        log_error "Fstab patch not found: ${FSTAB_PATCH}"
        exit 1
    fi
    patch -p1 -d "${KERNEL_DIR}" < "${FSTAB_PATCH}"

    log_success "Fstab ${VARIANT} variant applied via patch"
}

restore_fstab_variant() {
    if [ "${VARIANT}" = "dynamic" ]; then
        return
    fi
    log_step "Restoring original fstab configuration..."
    git -C "${KERNEL_DIR}" checkout -- \
        arch/arm64/boot/dts/qcom/sdm845-xiaomi-common.dtsi
    log_success "Original sdm845-xiaomi-common.dtsi restored"
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

    setup_toolchain
    setup_make_args

    for arg in "$@"; do
        if [ "${arg}" = "--release" ]; then
            run_release_builds
            exit 0
        fi
    done

    for arg in "$@"; do
        case "${arg}" in
            --tc-dir=*)
                TC_DIR="${arg#--tc-dir=}"
                setup_toolchain
                ;;
            --tc32-dir=*)
                TC32_DIR="${arg#--tc32-dir=}"
                setup_toolchain
                ;;
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

    BASE_KERNEL_NAME="${KERNEL_NAME}"

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

    trap 'restore_fstab_variant; restore_gpu_oc; restore_audio_configs' EXIT

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
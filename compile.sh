#!/bin/sh

# Basic build function
BUILD_START=$(date +"%s")
blue='\033[0;34m'
cyan='\033[0;36m'
yellow='\033[0;33m'
red='\033[0;31m'
nocol='\033[0m'

rmdir KernelSU
curl -LSs "https://raw.githubusercontent.com/rifsxd/KernelSU-Next/next-susfs-4.9/kernel/setup.sh" | bash -s next-susfs-4.9

git clone https://gitlab.com/simonpunk/susfs4ksu.git --depth 1 --branch kernel-4.9
cp -r susfs4ksu/kernel_patches/* .
patch -p1 < 50_add_susfs_in_kernel-4.9.patch

# Cleanup
rm -rf out/outputs/*

./compile-beryllium.sh

BUILD_END=$(date +"%s")
DIFF=$(($BUILD_END - $BUILD_START))
echo -e "$yellow Full build completed in $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds.$nocol"

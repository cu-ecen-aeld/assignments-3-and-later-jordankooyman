#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.
# Completed by: Jordan Kooyman
# Used DeepSeek to assist with TODO section completion: https://chat.deepseek.com/share/7p9wb4vuyfxistvdkd

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    # Clean previous builds (optional but recommended for fresh build)
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} distclean

    # Configure kernel with default aarch64 configuration
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

    # Or optionally use a custom config
    # cp /path/to/custom.config .config

    # Build the kernel
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all -j$(nproc)
fi

echo "Adding the Image in outdir"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
cd "$OUTDIR"
mkdir -p rootfs
cd rootfs
mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/lib usr/sbin 
mkdir -p var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    make distclean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    # Enable static linking to avoid library dependencies
    sed -i 's/.*CONFIG_STATIC.*/CONFIG_STATIC=y/' .config
    # Alternatively, configure interactively:
    # make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} menuconfig
else
    cd busybox
fi

# TODO: Make and install busybox
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} -j$(nproc)
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX=${OUTDIR}/rootfs install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
# Find the cross-compiler toolchain path
CROSS_COMPILER_PATH=$(dirname $(which ${CROSS_COMPILE}gcc))/../aarch64-none-linux-gnu/libc

# Copy the dynamic linker/loader
cp ${CROSS_COMPILER_PATH}/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib

# Copy required shared libraries
cp ${CROSS_COMPILER_PATH}/lib/libc.so.6 ${OUTDIR}/rootfs/lib
cp ${CROSS_COMPILER_PATH}/lib/libm.so.6 ${OUTDIR}/rootfs/lib
cp ${CROSS_COMPILER_PATH}/lib/libresolv.so.2 ${OUTDIR}/rootfs/lib
# TODO: Add other libraries as needed based on readelf output

# TODO: Make device nodes
cd "$OUTDIR/rootfs/dev"
sudo mknod -m 666 null c 1 3
sudo mknod -m 666 console c 5 1

# TODO: Clean and build the writer utility for aarch64
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
cd "$OUTDIR/rootfs"
mkdir -p home
cp ${FINDER_APP_DIR}/writer home/
cp ${FINDER_APP_DIR}/finder.sh home/
cp ${FINDER_APP_DIR}/finder-test.sh home/

# TODO: Chown the root directory
cd "${OUTDIR}/rootfs"
sudo chown -R root:root *

# TODO: Create initramfs.cpio.gz
cd "${OUTDIR}/rootfs"
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio

#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.
# Completed by: Jordan Kooyman
# Used DeepSeek to assist with TODO section completion and debugging: https://chat.deepseek.com/share/ne0ufdg67l7zvab9v8

# Install dependencies: sudo apt-get update && sudo apt-get install -y --no-install-recommends bc u-boot-tools kmod cpio flex bison libssl-dev psmisc && sudo apt-get install -y qemu-system-arm

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
    # Clean previous builds
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} distclean

    # First, get default configuration
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

    # Now set static libgcc BEFORE running oldconfig/olddefconfig
    echo "CONFIG_CC_STATIC_LIBGCC=y" >> .config

    # Update configuration non-interactively
    yes "" | make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} oldconfig

    # Build the kernel
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all -j$(nproc)
fi

echo "Copying kernel Image to ${OUTDIR}"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/Image

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
    
    # TODO: Configure busybox
    make distclean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    
    # Enable static linking
    echo "CONFIG_STATIC=y" >> .config
    
    # Set other options to avoid prompts
    echo "CONFIG_EXTRA_LDFLAGS=\"\"" >> .config
    echo "CONFIG_EXTRA_LDLIBS=\"\"" >> .config
    echo "CONFIG_USE_PORTABLE_CODE=n" >> .config
    echo "CONFIG_STACK_OPTIMIZATION_386=y" >> .config  # Default is y
    echo "CONFIG_STATIC_LIBGCC=y" >> .config  # The one that's prompting!
    
    # Update configuration non-interactively
    yes 'y' | make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} oldconfig
    
else
    cd busybox
fi

# TODO: Make and install busybox
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} -j$(nproc)
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX=${OUTDIR}/rootfs install

echo "Checking Library dependencies"
cd "$OUTDIR/rootfs"
if [ -f "bin/busybox" ]; then
    echo "Checking for dynamic dependencies..."
    if ${CROSS_COMPILE}readelf -a bin/busybox | grep -q "program interpreter"; then
        ${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
    else
        echo "Program interpreter: None (static binary)"
    fi
    
    if ${CROSS_COMPILE}readelf -a bin/busybox | grep -q "Shared library"; then
        ${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"
    else
        echo "Shared libraries: None (static binary)"
    fi
else
    echo "WARNING: BusyBox not found in rootfs/bin/"
fi

# TODO: Add library dependencies to rootfs
echo "Adding library dependencies to rootfs"
# Find the cross-compiler toolchain path
CROSS_COMPILER_PATH=$(dirname $(which ${CROSS_COMPILE}gcc))/../aarch64-none-linux-gnu/libc

# Copy the dynamic linker/loader
echo "Dynamic linker/loader"
cp ${CROSS_COMPILER_PATH}/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib

# Copy required shared libraries
echo "Copy shared libs"
# Function to copy a library if it exists
copy_lib() {
    local lib_name=$1
    local lib_path=$(${CROSS_COMPILE}gcc -print-file-name=$lib_name)
    if [ -f "$lib_path" ]; then
        echo "Copying $lib_name from $lib_path"
        cp "$lib_path" "${OUTDIR}/rootfs/lib/"
    else
        echo "WARNING: Could not find $lib_name"
    fi
}

# Copy essential libraries
copy_lib "ld-linux-aarch64.so.1"
copy_lib "libc.so.6"
copy_lib "libm.so.6"
copy_lib "libresolv.so.2"
#cp ${CROSS_COMPILER_PATH}/lib/libc.so.6 ${OUTDIR}/rootfs/lib
#cp ${CROSS_COMPILER_PATH}/lib/libm.so.6 ${OUTDIR}/rootfs/lib
#cp ${CROSS_COMPILER_PATH}/lib/libresolv.so.2 ${OUTDIR}/rootfs/lib
# TODO: Add other libraries as needed based on readelf output

# TODO: Make device nodes
echo "Make Device Nodes"
cd "$OUTDIR/rootfs/dev"
sudo mknod -m 666 null c 1 3
sudo mknod -m 666 console c 5 1

# TODO: Clean and build the writer utility for aarch64
echo "Builder writer for target platform"
cd ${FINDER_APP_DIR}
make clean
echo "Using Makefile with static flags..."
# Create a modified Makefile for static build
cp Makefile Makefile.backup
# Add -static to the linking stage
sed -i 's/$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)/$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -static/' Makefile 2>/dev/null || \
sed -i 's/^LDFLAGS =/LDFLAGS = -static/' Makefile 2>/dev/null || \
echo 'LDFLAGS += -static' >> Makefile

make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# Restore original Makefile
mv Makefile.backup Makefile

# Verify static linking
echo "Verifying writer is static:"
file writer
if file writer | grep -q "statically linked"; then
    echo "✓ Writer is statically linked"
else
    echo "✗ Writer is not static, trying alternative..."
    # Force static with all libraries
    ${CROSS_COMPILE}gcc -o writer writer.c \
        -static \
        -static-libgcc \
        -static-libstdc++ \
        -Wall \
        -Werror
fi

# Final verification
echo "Final check:"
file writer
${CROSS_COMPILE}readelf -d writer 2>/dev/null | grep "Shared library" || echo "No shared library dependencies (good!)"

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
echo "Copy finder-app assignment components to rootfs/home"
cd "$OUTDIR/rootfs"
mkdir -p home/conf
cp ${FINDER_APP_DIR}/writer home/
cp ${FINDER_APP_DIR}/finder.sh home/
cp ${FINDER_APP_DIR}/finder-test.sh home/
cp ${FINDER_APP_DIR}/autorun-qemu.sh home/
cp ${FINDER_APP_DIR}/conf/username.txt home/conf/
cp ${FINDER_APP_DIR}/conf/assignment.txt home/conf/

# TODO: Chown the root directory
echo "Update rootfs owner"
cd "${OUTDIR}/rootfs"
sudo chown -R root:root *

# TODO: Create initramfs.cpio.gz
echo "Creating initramfs.cpio.gz"
cd "${OUTDIR}/rootfs"
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio

echo "Done"

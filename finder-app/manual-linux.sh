#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if ! command -v ${CROSS_COMPILE}gcc >/dev/null 2>&1; then
    echo "WARNING: ${CROSS_COMPILE}gcc not found, falling back to aarch64-linux-gnu-"
    CROSS_COMPILE=aarch64-linux-gnu-
fi


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

    echo "Building the Linux kernel"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j"$(nproc)" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image

fi

echo "Adding the Image in outdir"

cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/Image

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

mkdir -p ${OUTDIR}/rootfs
cd ${OUTDIR}/rootfs

mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/sbin usr/lib


cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}

    make distclean
    make defconfig

else
    cd busybox
fi

make -j"$(nproc)" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "Shared library"

SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
echo "Using sysroot: ${SYSROOT}"

INTERP=$(${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter" | awk -F': ' '{print $2}' | tr -d '[]')
echo "Interpreter: ${INTERP}"

# If sysroot is "/", Ubuntu cross toolchains usually store target libs under multiarch paths
if [ "${SYSROOT}" = "/" ]; then
    LIBROOT="/usr/aarch64-linux-gnu"
    [ -d "${LIBROOT}" ] || LIBROOT=""
fi

# Copy interpreter
mkdir -p "${OUTDIR}/rootfs${INTERP%/*}"
if [ "${SYSROOT}" != "/" ] && [ -e "${SYSROOT}${INTERP}" ]; then
    cp -a "${SYSROOT}${INTERP}" "${OUTDIR}/rootfs${INTERP%/*}/"
elif [ -e "/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1" ]; then
    cp -a "/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1" "${OUTDIR}/rootfs/lib/"
else
    echo "ERROR: Could not find ld-linux-aarch64.so.1"
    exit 1
fi

# Copy shared libraries
LIBS=$(${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | \
    grep "Shared library" | awk -F'[][]' '{print $2}')

for L in ${LIBS}; do
    # Preferred Ubuntu cross-lib locations
    if [ -e "/usr/aarch64-linux-gnu/lib/${L}" ]; then
        cp -a "/usr/aarch64-linux-gnu/lib/${L}" "${OUTDIR}/rootfs/lib/"
    elif [ -e "/usr/aarch64-linux-gnu/lib64/${L}" ]; then
        cp -a "/usr/aarch64-linux-gnu/lib64/${L}" "${OUTDIR}/rootfs/lib64/"
    # Fallbacks for other toolchain layouts
    elif [ -e "${SYSROOT}/lib/${L}" ]; then
        cp -a "${SYSROOT}/lib/${L}" "${OUTDIR}/rootfs/lib/"
    elif [ -e "${SYSROOT}/lib64/${L}" ]; then
        cp -a "${SYSROOT}/lib64/${L}" "${OUTDIR}/rootfs/lib64/"
    else
        echo "ERROR: Library ${L} not found"
        echo "Tried: /usr/aarch64-linux-gnu/lib, /usr/aarch64-linux-gnu/lib64, ${SYSROOT}/lib, ${SYSROOT}/lib64"
        exit 1
    fi
done


sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1


echo "Building writer utility"
make -C ${FINDER_APP_DIR} clean
make -C ${FINDER_APP_DIR} CROSS_COMPILE=${CROSS_COMPILE}
cp ${FINDER_APP_DIR}/writer ${OUTDIR}/rootfs/home/


echo "Copying finder app scripts and configuration to rootfs"

cp ${FINDER_APP_DIR}/finder.sh ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home/

mkdir -p ${OUTDIR}/rootfs/home/conf
cp ${FINDER_APP_DIR}/../conf/username.txt ${OUTDIR}/rootfs/home/conf/
cp ${FINDER_APP_DIR}/../conf/assignment.txt ${OUTDIR}/rootfs/home/conf/

sed -i 's|\.\./conf/assignment.txt|conf/assignment.txt|g' ${OUTDIR}/rootfs/home/finder-test.sh


sudo chown -R root:root ${OUTDIR}/rootfs

echo "Creating initramfs"
cd ${OUTDIR}/rootfs
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio
echo "Created ${OUTDIR}/initramfs.cpio.gz"


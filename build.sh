#!/bin/bash -x

setup ()
{
    if [ x = "x$ANDROID_BUILD_TOP" ] ; then
        echo "Android build environment must be configured."
        exit 1
    fi
    . "$ANDROID_BUILD_TOP"/build/envsetup.sh

    KERNEL_DIR="$(dirname "$(readlink -f "$0")")"
    MODULES=("crypto/pcbc.ko" "drivers/bluetooth/bthid/bthid.ko" "drivers/media/video/gspca/gspca_main.ko" "drivers/media/video/omapgfx/gfx_vout_mod.ko" \
        "drivers/scsi/scsi_wait_scan.ko" "drivers/net/wireless/bcm4330/dhd.ko" "drivers/staging/ti-st/bt_drv.ko" "drivers/staging/omap_hsi/hsi_char.ko" \
        "drivers/staging/ti-st/fm_drv.ko" "drivers/staging/ti-st/gps_drv.ko" "drivers/staging/ti-st/st_drv.ko" "samsung/fm_si4709/Si4709_driver.ko" \
        "samsung/param/param.ko" "samsung/j4fs/j4fs.ko" "samsung/vibetonz/vibetonz.ko")


    if [ x = "x$NO_CCACHE" ] && ccache -V &>/dev/null ; then
        CCACHE=ccache
        CCACHE_BASEDIR="$KERNEL_DIR"
        CCACHE_COMPRESS=1
        CCACHE_DIR="$KERNEL_DIR/.ccache"
        export CCACHE_DIR CCACHE_COMPRESS CCACHE_BASEDIR
    else
        CCACHE=""
    fi

    CROSS_PREFIX="$ANDROID_BUILD_TOP/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-"
}

build ()
{
    local target=$1
    echo "Building for $target"
    local module
    mka mrproper CROSS_COMPILE="$CCACHE $CROSS_PREFIX"
    [ x = "x$NO_DEFCONFIG" ] && mka -C "$KERNEL_DIR" android_${target}_defconfig ARCH=arm HOSTCC="$CCACHE gcc"
    if [ x = "x$NO_BUILD" ] ; then
        mka -C "$KERNEL_DIR" ARCH=arm HOSTCC="$CCACHE gcc" CROSS_COMPILE="$CCACHE $CROSS_PREFIX" modules
        mka -C "$KERNEL_DIR" ARCH=arm HOSTCC="$CCACHE gcc" CROSS_COMPILE="$CCACHE $CROSS_PREFIX" zImage
        cp "$KERNEL_DIR"/arch/arm/boot/zImage $ANDROID_BUILD_TOP/device/samsung/$target/zImage
        for module in "${MODULES[@]}" ; do
            cp "$KERNEL_DIR/$module" $ANDROID_BUILD_TOP/device/samsung/$target/modules
        done
    fi
}
    
setup

if [ "$1" = clean ] ; then
    mka mrproper CROSS_COMPILE="$CCACHE $CROSS_PREFIX"
    exit 0
fi

targets=("$@")
if [ 0 = "${#targets[@]}" ] ; then
    targets=(i9100g)
fi

START=$(date +%s)

for target in "${targets[@]}" ; do 
    build $target
done

END=$(date +%s)
ELAPSED=$((END - START))
E_MIN=$((ELAPSED / 60))
E_SEC=$((ELAPSED - E_MIN * 60))
printf "Elapsed: "
[ $E_MIN != 0 ] && printf "%d min(s) " $E_MIN
printf "%d sec(s)\n" $E_SEC

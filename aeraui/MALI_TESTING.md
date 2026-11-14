# Mali testing

This branch adds a stock-driver Mali backend to AERA `aera-16.0`. The base is
upstream `e438f6de62090a79e81daf00b449b48da7377753`, including the merged slot
controls, adaptive framebuffer, browser startup, terminal layout and
pattern-drawing fixes.

Tested: Huawei P30 lite, Kirin 710 / Mali-G51, EMUI 9.1.0.399, Android 9
graphics libraries and the stock 4.9.148 kernel.

The backend needs **gralloc1 and HWC2 HAL modules**. GPU model alone does not
establish compatibility. Gralloc4 and HIDL/AIDL-only composer services need
another integration path. Mali-G57 is untested.

## Build

Start with an Android 16 AERA checkout and your own booting device tree.
Use clean recovery and LVGL checkouts. From the Android source root:

```sh
git -C bootable/recovery fetch https://github.com/apkreader/android_bootable_recovery.git mali-testing
git -C bootable/recovery switch --detach FETCH_HEAD
python3 bootable/recovery/aeraui/tools/prepare-mali-lvgl.py
```

The helper checks the complete sequence before applying it and skips patches
that are already applied. The LVGL base is
`626794146201c14eb70e030ae188fcaa6c7d376f`.
Use `--check` to check compatibility without changing the checkout.

Configure the stock graphics libraries, then use your device's normal lunch
target and build command:

```sh
source build/envsetup.sh
lunch <your-device-target>
m recoveryimage
```

Use your device's installation method and keep its working recovery available.
The P30 lite tree is a hardware setup example.

## Device setup

Make the matching stock EGL/GLES driver, gralloc1 HAL, HWC2 HAL and their
dependencies available before AERA starts. Set these properties through
your device's recovery properties or startup script:

```text
ro.aera.gpu.mali_driver=<absolute path to the stock EGL/GLES library>
ro.aera.gpu.mali_libraries=<colon-separated dependency directories>
ro.aera.gpu.gralloc=<absolute path to the gralloc1 HAL>
ro.aera.gpu.hwcomposer=<absolute path to the HWC2 HAL>
```

The remaining options are under [Legacy Mali](README.md#legacy-mali).
Leave native fences disabled for the first test.

The reference tree is
[`android_device_huawei_marie`, branch `mali-testing`](https://github.com/apkreader/android_device_huawei_marie/tree/mali-testing).
Its `marie-gpu-launch.sh` and `marie-gpu-libraries.sh` stage matching EMUI
libraries. Other devices need their own paths and HALs; the P30 lite's fence
ownership setting is specific to its composer.

Without the Mali driver property, AERA keeps its existing graphics path.
Failed Mali initialization falls back to software rendering. A booting UI
alone does not confirm acceleration.

## Test and report

Look for `Mali HWC2 GPU ready` and your renderer name:

```sh
adb logcat -d -b all -s AeraGpu > aera-gpu.txt
adb pull /tmp/recovery.log aera-recovery.log
```

Check colors, orientation, touch, Settings scrolling, tab changes and
sleep/wake. Report the model, SoC/GPU, Android/vendor version, driver and HAL
paths, and whether the GPU-ready line appears. Attach the logs and describe
the first visible problem.

This branch covers recovery UI rendering. The experimental Browser bridge is
kept separately.

#!/usr/bin/env python3
# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--android-root', type=Path, required=True)
args = parser.parse_args()
root = args.android_root.resolve()
source = Path(__file__).resolve().parents[2]
assert (root / 'hardware/libhardware/include/hardware/gralloc1.h').is_file()
with tempfile.TemporaryDirectory(prefix='aera-gpu-backends-') as temporary:
    build = Path(temporary)
    stubs = build / 'stubs'
    for name, content in {
        'android-base/properties.h': '#pragma once\n#include <string>\nnamespace android::base { std::string GetProperty(const std::string&, const std::string&); }\n',
        'android/dlext.h': '#pragma once\n#include <stdint.h>\nstruct android_namespace_t;\nstruct android_dlextinfo { uint64_t flags; android_namespace_t* library_namespace; };\n#define ANDROID_DLEXT_USE_NAMESPACE 0x200\nextern "C" void* android_dlopen_ext(const char*, int, const android_dlextinfo*);\n',
        'android/log.h': '#pragma once\n#define ANDROID_LOG_WARN 5\nextern "C" int __android_log_print(int, const char*, const char*, ...);\n',
    }.items():
        path = stubs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    host_args = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
                 '-I' + str(stubs), '-Ihardware/libhardware/include',
                 '-Iframeworks/native/libs/nativebase/include',
                 '-Isystem/core/libcutils/include', '-Isystem/core/libsystem/include',
                 '-Isystem/core/libutils/include', '-Isystem/libhidl/base/include',
                 '-Wl,--wrap=dlopen,--wrap=dlsym,--wrap=dlclose',
                 str(source / 'aeraui/core/mali_display.cpp'),
                 str(source / 'aeraui/tests/mali_display_test.cpp'),
                 '-o', str(build / 'mali-display-test')]
    subprocess.run(host_args, cwd=root, check=True)
    subprocess.run([str(build / 'mali-display-test')], check=True, timeout=30)
    print('Mali HAL failure, fence, disconnect and cleanup checks passed', flush=True)

    for name, content in {
        'android/hardware_buffer.h': '''#pragma once
    #include <stdint.h>
    #include <cutils/native_handle.h>
    struct AHardwareBuffer;
    struct AHardwareBuffer_Desc { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; };
    #define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
    #define AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER (1ULL << 9)
    #define AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY (1ULL << 11)
    #define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 3
    #define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN (3ULL << 4)
    ''',
        'vndk/hardware_buffer.h': '#pragma once\n#include <android/hardware_buffer.h>\n',
        'pixelflinger/pixelflinger.h': '#pragma once\n',
    }.items():
        path = stubs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    with (stubs / 'android/log.h').open('a') as stream:
        stream.write('#define ANDROID_LOG_ERROR 6\n#define ANDROID_LOG_INFO 4\n')
    glad = root / 'external/lvgl/src/drivers/opengles/glad'
    glad_objects = []
    for name in ('egl', 'gles2'):
        target = build / (name + '.o')
        subprocess.run(['cc', '-DLV_CONF_INCLUDE_SIMPLE', '-I' + str(root / 'external/lvgl'),
                        '-I' + str(root / 'frameworks/native/opengl/include'),
                        '-I' + str(glad / 'include'), '-c', str(glad / 'src' / (name + '.c')),
                        '-o', str(target)], check=True)
        glad_objects.append(str(target))
    subprocess.run(['c++', '-std=c++17', '-UNDEBUG', '-DLV_CONF_INCLUDE_SIMPLE', '-I' + str(stubs),
        '-I' + str(root / 'external/lvgl'), '-I' + str(root / 'external/lvgl/src'),
        '-I' + str(root / 'frameworks/native/opengl/include'),
        '-I' + str(root / 'bootable/recovery/minuitwrp/include'),
        '-I' + str(root / 'system/core/libcutils/include'),
        str(source / 'aeraui/core/gpu_renderer.cpp'),
        str(source / 'aeraui/tests/gpu_renderer_test.cpp'), *glad_objects,
        '-Wl,--wrap=dlopen,--wrap=dlsym,--wrap=dlclose,--wrap=gladLoadEGLUserPtr,--wrap=gladLoadGLES2UserPtr',
        '-ldl', '-o', str(build / 'gpu-renderer-test')], cwd=root, check=True)
    subprocess.run([str(build / 'gpu-renderer-test')], check=True, timeout=30)
    print('Adreno, Mali, failed setup and display cleanup renderer checks passed', flush=True)

/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../core/mali_display.hpp"

#include <android/dlext.h>
#include <hardware/gralloc1.h>
#include <hardware/gralloc.h>
#include <hardware/hwcomposer2.h>
#include <nativebase/nativebase.h>
#include <dirent.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdarg>
#include <cstring>
#include <map>
#include <string>

namespace {
std::map<std::string, std::string> properties;
int missing_function = -1;
bool fail_namespace = false;
bool fail_driver = false;
bool fail_allocation = false;
bool fail_present = false;
bool change_layer = false;
int allocations = 0, releases = 0, descriptors = 0;
int allocator_closes = 0, composer_closes = 0, layers = 0, libraries = 0;
int pending_fence = -1;
int layer_fence = -1;
bool fail_release_fences = false;
hwc2_callback_data_t callback_data = nullptr;
HWC2_PFN_HOTPLUG hotplug = nullptr;
gralloc1_device_t allocator{};
hwc2_device_t composer{};
native_handle_t handles[3]{};
hw_module_t allocator_module{}, composer_module{};

int CloseAllocator(hw_device_t*) { ++allocator_closes; return 0; }
int CloseComposer(hw_device_t*) { ++composer_closes; return 0; }
int OpenAllocator(const hw_module_t*, const char*, hw_device_t** out) {
  *out = &allocator.common; return 0;
}
int OpenComposer(const hw_module_t*, const char*, hw_device_t** out) {
  *out = &composer.common; return 0;
}
int CreateDescriptor(gralloc1_device_t*, gralloc1_buffer_descriptor_t* out) {
  ++descriptors; *out = 1; return 0;
}
int DestroyDescriptor(gralloc1_device_t*, gralloc1_buffer_descriptor_t) {
  --descriptors; return 0;
}
int Dimensions(gralloc1_device_t*, gralloc1_buffer_descriptor_t, uint32_t, uint32_t) { return 0; }
int Format(gralloc1_device_t*, gralloc1_buffer_descriptor_t, int32_t) { return 0; }
int Usage(gralloc1_device_t*, gralloc1_buffer_descriptor_t, uint64_t) { return 0; }
int Allocate(gralloc1_device_t*, uint32_t, const gralloc1_buffer_descriptor_t*, buffer_handle_t* out) {
  if (fail_allocation && allocations == 1) return GRALLOC1_ERROR_NO_RESOURCES;
  *out = &handles[allocations++]; return 0;
}
int Stride(gralloc1_device_t*, buffer_handle_t, uint32_t* out) { *out = 1088; return 0; }
int Release(gralloc1_device_t*, buffer_handle_t) { ++releases; return 0; }
gralloc1_function_pointer_t AllocatorFunction(gralloc1_device_t*, int32_t id) {
  if (id == missing_function) return nullptr;
#define FUNCTION(name, value) case GRALLOC1_FUNCTION_##name: return reinterpret_cast<gralloc1_function_pointer_t>(value)
  switch (id) {
    FUNCTION(CREATE_DESCRIPTOR, CreateDescriptor);
    FUNCTION(DESTROY_DESCRIPTOR, DestroyDescriptor);
    FUNCTION(SET_DIMENSIONS, Dimensions);
    FUNCTION(SET_FORMAT, Format);
    FUNCTION(SET_PRODUCER_USAGE, Usage);
    FUNCTION(SET_CONSUMER_USAGE, Usage);
    FUNCTION(ALLOCATE, Allocate);
    FUNCTION(GET_STRIDE, Stride);
    FUNCTION(RELEASE, Release);
    default: return nullptr;
  }
#undef FUNCTION
}
int Callback(hwc2_device_t*, int32_t id, hwc2_callback_data_t data, hwc2_function_pointer_t function) {
  assert(id == HWC2_CALLBACK_HOTPLUG);
  callback_data = data;
  hotplug = reinterpret_cast<HWC2_PFN_HOTPLUG>(function);
  if (hotplug) hotplug(data, 7, HWC2_CONNECTION_CONNECTED);
  return 0;
}
int Power(hwc2_device_t*, hwc2_display_t id, int32_t) { assert(id == 7); return 0; }
int Layer(hwc2_device_t*, hwc2_display_t, hwc2_layer_t* out) { ++layers; *out = 9; return 0; }
int DestroyLayer(hwc2_device_t*, hwc2_display_t, hwc2_layer_t) { --layers; return 0; }
int Composition(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t) { return 0; }
int Frame(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_rect_t) { return 0; }
int Crop(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_frect_t) { return 0; }
int Alpha(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, float) { return 0; }
int Target(hwc2_device_t*, hwc2_display_t, buffer_handle_t, int32_t fence, int32_t, hwc_region_t) {
  if (fence >= 0) {
    assert(fcntl(fence, F_GETFD) >= 0);
    if (properties["ro.aera.gpu.hwc_acquire_fence_ownership"] == "composer")
      close(fence);
  }
  return 0;
}
int Validate(hwc2_device_t*, hwc2_display_t, uint32_t* types, uint32_t* requests) {
  *types = change_layer ? 1 : 0; *requests = 0; return 0;
}
int Accept(hwc2_device_t*, hwc2_display_t) { return 0; }
int Present(hwc2_device_t*, hwc2_display_t, int32_t* out) {
  *out = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  assert(*out >= 0);
  pending_fence = *out;
  return fail_present ? HWC2_ERROR_BAD_DISPLAY : 0;
}
int ReleaseFences(hwc2_device_t*, hwc2_display_t, uint32_t* count,
                  hwc2_layer_t* out_layers, int32_t* out_fences) {
  if (!out_layers || !out_fences) { *count = 1; return 0; }
  assert(*count >= 1);
  *count = 1;
  *out_layers = 9;
  *out_fences = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  assert(*out_fences >= 0);
  layer_fence = *out_fences;
  return fail_release_fences ? HWC2_ERROR_BAD_DISPLAY : 0;
}
int DescriptorCount() {
  DIR* directory = opendir("/proc/self/fd");
  assert(directory);
  int count = 0;
  while (readdir(directory)) ++count;
  closedir(directory);
  return count;
}
hwc2_function_pointer_t ComposerFunction(hwc2_device_t*, int32_t id) {
#define FUNCTION(name, value) case HWC2_FUNCTION_##name: return reinterpret_cast<hwc2_function_pointer_t>(value)
  switch (id) {
    FUNCTION(REGISTER_CALLBACK, Callback);
    FUNCTION(CREATE_LAYER, Layer);
    FUNCTION(DESTROY_LAYER, DestroyLayer);
    FUNCTION(SET_POWER_MODE, Power);
    FUNCTION(SET_LAYER_COMPOSITION_TYPE, Composition);
    FUNCTION(SET_LAYER_DISPLAY_FRAME, Frame);
    FUNCTION(SET_LAYER_SOURCE_CROP, Crop);
    FUNCTION(SET_LAYER_BLEND_MODE, Composition);
    FUNCTION(SET_LAYER_PLANE_ALPHA, Alpha);
    FUNCTION(SET_CLIENT_TARGET, Target);
    FUNCTION(VALIDATE_DISPLAY, Validate);
    FUNCTION(ACCEPT_DISPLAY_CHANGES, Accept);
    FUNCTION(PRESENT_DISPLAY, Present);
    FUNCTION(GET_RELEASE_FENCES, ReleaseFences);
    default: return nullptr;
  }
#undef FUNCTION
}
android_namespace_t* CreateNamespace(const char*, const char*, const char*, uint64_t, const char*, android_namespace_t*) {
  return fail_namespace ? nullptr : reinterpret_cast<android_namespace_t*>(1);
}
bool LinkNamespace(android_namespace_t*, android_namespace_t*, const char*) { return true; }

void Reset() {
  assert(libraries == 0 && layers == 0 && descriptors == 0);
  assert(allocations == releases);
  missing_function = -1;
  fail_namespace = fail_driver = fail_allocation = fail_present = change_layer = false;
  fail_release_fences = false;
  allocations = releases = allocator_closes = composer_closes = 0;
  callback_data = nullptr; hotplug = nullptr; pending_fence = -1;
  allocator.common.close = CloseAllocator;
  allocator.getFunction = AllocatorFunction;
  composer.common.close = CloseComposer;
  composer.common.version = HWC_DEVICE_API_VERSION_2_0;
  composer.getFunction = ComposerFunction;
  static hw_module_methods_t allocator_methods{OpenAllocator};
  static hw_module_methods_t composer_methods{OpenComposer};
  allocator_module.methods = &allocator_methods;
  composer_module.methods = &composer_methods;
  properties = {{"ro.aera.gpu.mali_driver", "/mali.so"},
                {"ro.aera.gpu.mali_libraries", "/libraries"},
                {"ro.aera.gpu.gralloc", "/gralloc.so"},
                {"ro.aera.gpu.hwcomposer", "/composer.so"}};
}
}  // namespace

namespace android::base {
std::string GetProperty(const std::string& key, const std::string& fallback) {
  const auto found = properties.find(key);
  return found == properties.end() ? fallback : found->second;
}
}  // namespace android::base

extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" void* __wrap_dlopen(const char*, int) { ++libraries; return reinterpret_cast<void*>(1); }
extern "C" int __wrap_dlclose(void*) { --libraries; return 0; }
extern "C" void* android_dlopen_ext(const char* name, int, const android_dlextinfo*) {
  if (!std::strcmp(name, "/mali.so") && fail_driver) return nullptr;
  ++libraries;
  return reinterpret_cast<void*>(!std::strcmp(name, "/gralloc.so") ? 2 :
                                !std::strcmp(name, "/composer.so") ? 3 : 4);
}
extern "C" void* __wrap_dlsym(void* library, const char* name) {
  if (!std::strcmp(name, "android_create_namespace")) return reinterpret_cast<void*>(CreateNamespace);
  if (!std::strcmp(name, "android_link_namespaces")) return reinterpret_cast<void*>(LinkNamespace);
  if (!std::strcmp(name, HAL_MODULE_INFO_SYM_AS_STR))
    return library == reinterpret_cast<void*>(2) ? &allocator_module : &composer_module;
  return nullptr;
}

int main() {
  Reset();
  {
    aeraui::MaliDisplay display;
    properties.erase("ro.aera.gpu.gralloc");
    assert(!display.OpenDriver());
    assert(!display.Buffer(0) && !display.Wait(0) && !display.Present(0));
  }
  Reset();
  { aeraui::MaliDisplay display; fail_namespace = true; assert(!display.OpenDriver()); }
  Reset();
  { aeraui::MaliDisplay display; fail_driver = true; assert(!display.OpenDriver()); }
  for (int function : {GRALLOC1_FUNCTION_CREATE_DESCRIPTOR, GRALLOC1_FUNCTION_DESTROY_DESCRIPTOR,
                       GRALLOC1_FUNCTION_SET_DIMENSIONS, GRALLOC1_FUNCTION_SET_FORMAT,
                       GRALLOC1_FUNCTION_SET_PRODUCER_USAGE, GRALLOC1_FUNCTION_SET_CONSUMER_USAGE,
                       GRALLOC1_FUNCTION_ALLOCATE, GRALLOC1_FUNCTION_GET_STRIDE, GRALLOC1_FUNCTION_RELEASE}) {
    Reset();
    aeraui::MaliDisplay display;
    missing_function = function;
    assert(display.OpenDriver());
    assert(!display.Initialize(1080, 2312));
  }
  Reset();
  {
    aeraui::MaliDisplay display;
    assert(display.OpenDriver());
    fail_allocation = true;
    assert(!display.Initialize(1080, 2312));
  }
  Reset();
  {
    aeraui::MaliDisplay display;
    assert(display.OpenDriver());
    assert(!display.Initialize(0, 2312));
    assert(display.Initialize(1080, 2312));
    assert(!display.Initialize(1080, 2312));
    assert(display.Buffer(0) && display.Buffer(1) && !display.Buffer(2));
    const auto* native = static_cast<ANativeWindowBuffer*>(display.Buffer(0));
    assert(native->usage == (GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER |
                            GRALLOC_USAGE_HW_FB | GRALLOC_USAGE_SW_READ_OFTEN |
                            GRALLOC_USAGE_SW_WRITE_OFTEN));
    assert(display.Wait(0) && display.Present(0));
    assert(fcntl(pending_fence, F_GETFD) == -1);
    assert(fcntl(layer_fence, F_GETFD) == -1);
    assert(!display.Present(0));
    assert(display.Wait(1) && display.Present(1));
    const int release_fence = pending_fence;
    assert(!display.Wait(0));
    const uint64_t signal = 1;
    assert(write(release_fence, &signal, sizeof(signal)) == sizeof(signal));
    assert(display.Wait(0));
    assert(fcntl(release_fence, F_GETFD) == -1);
    fail_present = true;
    assert(!display.Present(0));
    assert(fcntl(pending_fence, F_GETFD) == -1);
    fail_present = false;
    change_layer = true;
    assert(!display.Present(0));
    hotplug(callback_data, 7, HWC2_CONNECTION_DISCONNECTED);
    assert(!display.Wait(0) && !display.Present(0));
  }
  assert(allocator_closes == 1 && composer_closes == 1 && !hotplug);
  Reset();
  const int descriptor_count = DescriptorCount();
  for (unsigned buffer_count : {2U, 3U}) {
    properties["ro.aera.gpu.mali_buffers"] = std::to_string(buffer_count);
    {
      aeraui::MaliDisplay display;
      assert(display.OpenDriver() && display.Initialize(1080, 2312));
      assert(display.BufferCount() == buffer_count);
      assert(display.Buffer(buffer_count - 1) && !display.Buffer(buffer_count));
      for (unsigned frame = 0; frame < 500; ++frame) {
        if (pending_fence >= 0 && fcntl(pending_fence, F_GETFD) >= 0) {
          const uint64_t signal = 1;
          assert(write(pending_fence, &signal, sizeof(signal)) == sizeof(signal));
        }
        assert(display.Wait(frame % buffer_count));
        assert(display.Present(frame % buffer_count));
        assert(fcntl(layer_fence, F_GETFD) == -1);
        assert(DescriptorCount() <= descriptor_count + static_cast<int>(buffer_count) - 1);
      }
    }
    assert(DescriptorCount() == descriptor_count);
    Reset();
  }
  assert(DescriptorCount() == descriptor_count);
  Reset();
  {
    aeraui::MaliDisplay display;
    assert(display.OpenDriver() && display.Initialize(1080, 2312));
    fail_release_fences = true;
    assert(!display.Present(0));
    assert(fcntl(layer_fence, F_GETFD) == -1);
  }
  assert(DescriptorCount() == descriptor_count);
  Reset();
  for (const char* ownership : {"caller", "composer"}) {
    properties["ro.aera.gpu.hwc_acquire_fence_ownership"] = ownership;
    {
      aeraui::MaliDisplay display;
      assert(display.OpenDriver() && display.Initialize(1080, 2312));
      int acquire = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      assert(acquire >= 0 && display.Present(0, acquire));
      assert(fcntl(acquire, F_GETFD) == -1);
      acquire = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      assert(acquire >= 0 && !display.Present(2, acquire));
      assert(fcntl(acquire, F_GETFD) == -1);
    }
    assert(DescriptorCount() == descriptor_count);
    Reset();
  }
}

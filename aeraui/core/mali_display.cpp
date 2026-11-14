/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mali_display.hpp"

#include <android-base/properties.h>
#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <hardware/gralloc.h>
#include <hardware/gralloc1.h>
#include <hardware/hwcomposer2.h>
#include <nativebase/nativebase.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace aeraui {
namespace {
constexpr char kLogTag[] = "AeraGpu";
constexpr unsigned kMaxBufferCount = 3;
constexpr hwc2_display_t kNoDisplay = std::numeric_limits<hwc2_display_t>::max();
constexpr uint64_t kProducerUsage =
    GRALLOC1_PRODUCER_USAGE_GPU_RENDER_TARGET |
    GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN |
    GRALLOC1_PRODUCER_USAGE_CPU_READ_OFTEN;
constexpr uint64_t kConsumerUsage =
    GRALLOC1_CONSUMER_USAGE_HWCOMPOSER |
    GRALLOC1_CONSUMER_USAGE_CLIENT_TARGET |
    GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN;
constexpr uint64_t kNativeUsage = GRALLOC_USAGE_HW_RENDER |
    GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_FB |
    GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;

template <typename T, typename Device>
bool GetFunction(Device* device, int32_t descriptor, T* function) {
  *function = reinterpret_cast<T>(device->getFunction(device, descriptor));
  return *function != nullptr;
}

bool Fail(const char* operation) {
  __android_log_print(ANDROID_LOG_WARN, kLogTag, "Mali: %s failed", operation);
  return false;
}
}  // namespace

struct MaliDisplay::Impl {
  void* linker_library = nullptr;
  void* driver_library = nullptr;
  void* allocator_library = nullptr;
  void* composer_library = nullptr;
  android_namespace_t* driver_namespace = nullptr;
  gralloc1_device_t* allocator = nullptr;
  hwc2_device_t* composer = nullptr;
  buffer_handle_t buffers[kMaxBufferCount] = {};
  ANativeWindowBuffer native[kMaxBufferCount] = {};
  int fences[kMaxBufferCount] = {-1, -1, -1};
  unsigned buffer_count = 2;
  int last_buffer = -1;
  int width = 0;
  int height = 0;
  std::atomic<hwc2_display_t> display{kNoDisplay};
  std::atomic<bool> connected{false};
  hwc2_layer_t layer = 0;
  bool has_layer = false;
  bool registered = false;
  bool ready = false;
  bool composer_owns_acquire_fence = false;
  GRALLOC1_PFN_RELEASE release = nullptr;
  HWC2_PFN_REGISTER_CALLBACK register_callback = nullptr;
  HWC2_PFN_SET_CLIENT_TARGET set_target = nullptr;
  HWC2_PFN_VALIDATE_DISPLAY validate = nullptr;
  HWC2_PFN_ACCEPT_DISPLAY_CHANGES accept = nullptr;
  HWC2_PFN_PRESENT_DISPLAY present = nullptr;
  HWC2_PFN_DESTROY_LAYER destroy_layer = nullptr;
  HWC2_PFN_GET_RELEASE_FENCES get_release_fences = nullptr;

  bool ReleaseLayerFences() {
    uint32_t count = 0;
    if (get_release_fences(composer, display.load(), &count, nullptr, nullptr))
      return Fail("release fence count");
    if (count == 0) return true;
    if (count > 1024) return Fail("invalid release fence count");
    std::vector<hwc2_layer_t> layers(count);
    std::vector<int32_t> returned_fences(count, -1);
    const int result = get_release_fences(composer, display.load(), &count,
                                         layers.data(), returned_fences.data());
    // No layer buffer producer receives these descriptors; release our copies.
    for (const int fd : returned_fences) {
      if (fd >= 0) close(fd);
    }
    return result == HWC2_ERROR_NONE || Fail("layer release fences");
  }

  static void Reference(android_native_base_t*) {}

  static void Hotplug(hwc2_callback_data_t data, hwc2_display_t id,
                      int32_t state) {
    auto* self = static_cast<Impl*>(data);
    if (state == HWC2_CONNECTION_CONNECTED) {
      hwc2_display_t expected = kNoDisplay;
      self->display.compare_exchange_strong(expected, id);
      if (self->display.load() == id) self->connected.store(true);
    } else if (self->display.load() == id) {
      self->connected.store(false);
    }
  }

  void* Load(const std::string& path) {
    android_dlextinfo info{};
    info.flags = ANDROID_DLEXT_USE_NAMESPACE;
    info.library_namespace = driver_namespace;
    return android_dlopen_ext(path.c_str(), RTLD_NOW, &info);
  }

  ~Impl() {
    if (registered)
      register_callback(composer, HWC2_CALLBACK_HOTPLUG, nullptr, nullptr);
    if (has_layer && destroy_layer)
      destroy_layer(composer, display.load(), layer);
    if (composer) composer->common.close(&composer->common);
    for (unsigned i = 0; i < buffer_count; ++i) {
      if (fences[i] >= 0) close(fences[i]);
      if (buffers[i] && release) release(allocator, buffers[i]);
    }
    if (allocator) gralloc1_close(allocator);
    if (composer_library) dlclose(composer_library);
    if (allocator_library) dlclose(allocator_library);
    if (driver_library) dlclose(driver_library);
    if (linker_library) dlclose(linker_library);
  }
};

MaliDisplay::MaliDisplay() : impl_(new Impl) {}
MaliDisplay::~MaliDisplay() = default;

void* MaliDisplay::OpenDriver() {
  auto& state = *impl_;
  if (state.driver_library) return state.driver_library;
  const auto driver = android::base::GetProperty("ro.aera.gpu.mali_driver", "");
  const auto libraries = android::base::GetProperty("ro.aera.gpu.mali_libraries", "");
  const auto gralloc = android::base::GetProperty("ro.aera.gpu.gralloc", "");
  const auto composer = android::base::GetProperty("ro.aera.gpu.hwcomposer", "");
  if (driver.empty() || libraries.empty() || gralloc.empty() || composer.empty()) {
    Fail("driver configuration");
    return nullptr;
  }
  const auto linker = android::base::GetProperty("ro.aera.gpu.mali_linker", "libdl_android.so");
  const auto permitted = android::base::GetProperty("ro.aera.gpu.mali_permitted_paths", "/vendor");
  state.linker_library = dlopen(linker.c_str(), RTLD_NOW);
  using CreateNamespace = android_namespace_t* (*)(const char*, const char*,
      const char*, uint64_t, const char*, android_namespace_t*);
  using LinkNamespace = bool (*)(android_namespace_t*, android_namespace_t*, const char*);
  auto create = state.linker_library ? reinterpret_cast<CreateNamespace>(
      dlsym(state.linker_library, "android_create_namespace")) : nullptr;
  auto link = state.linker_library ? reinterpret_cast<LinkNamespace>(
      dlsym(state.linker_library, "android_link_namespaces")) : nullptr;
  if (!create || !link) {
    Fail("linker namespace API");
    return nullptr;
  }
  state.driver_namespace = create("aera_mali", libraries.c_str(), libraries.c_str(),
                                  1, permitted.c_str(), nullptr);
  if (!state.driver_namespace ||
      !link(state.driver_namespace, nullptr, "libc.so:libm.so:libdl.so")) {
    Fail("driver namespace");
    return nullptr;
  }
  state.driver_library = state.Load(driver);
  if (!state.driver_library) Fail("EGL driver loading");
  return state.driver_library;
}

bool MaliDisplay::Initialize(int width, int height) {
  auto& state = *impl_;
  if (!state.driver_library || state.allocator || state.composer ||
      width <= 0 || height <= 0) return false;
  state.width = width;
  state.height = height;
  state.buffer_count = android::base::GetProperty("ro.aera.gpu.mali_buffers", "2") == "3" ? 3 : 2;
  state.composer_owns_acquire_fence = android::base::GetProperty(
      "ro.aera.gpu.hwc_acquire_fence_ownership", "caller") == "composer";
  state.allocator_library = state.Load(android::base::GetProperty("ro.aera.gpu.gralloc", ""));
  state.composer_library = state.Load(android::base::GetProperty("ro.aera.gpu.hwcomposer", ""));
  if (!state.allocator_library || !state.composer_library)
    return Fail("display HAL loading");
  auto* module = static_cast<hw_module_t*>(dlsym(state.allocator_library, HAL_MODULE_INFO_SYM_AS_STR));
  if (!module || !module->methods || !module->methods->open ||
      gralloc1_open(module, &state.allocator) != 0 || !state.allocator ||
      !state.allocator->getFunction) return Fail("gralloc1 open");
  module = static_cast<hw_module_t*>(dlsym(state.composer_library, HAL_MODULE_INFO_SYM_AS_STR));
  if (!module || !module->methods || !module->methods->open ||
      module->methods->open(module, HWC_HARDWARE_COMPOSER,
          reinterpret_cast<hw_device_t**>(&state.composer)) != 0 ||
      !state.composer)
    return Fail("HWC2 open");
  if ((state.composer->common.version >> 24) != 2 || !state.composer->getFunction)
    return Fail("HWC2 device version");

#define ALLOCATOR(type, name) \
  type name = nullptr; \
  if (!GetFunction(state.allocator, GRALLOC1_FUNCTION_##name, &name)) \
    return Fail("gralloc1 " #name)
  ALLOCATOR(GRALLOC1_PFN_CREATE_DESCRIPTOR, CREATE_DESCRIPTOR);
  ALLOCATOR(GRALLOC1_PFN_DESTROY_DESCRIPTOR, DESTROY_DESCRIPTOR);
  ALLOCATOR(GRALLOC1_PFN_SET_DIMENSIONS, SET_DIMENSIONS);
  ALLOCATOR(GRALLOC1_PFN_SET_FORMAT, SET_FORMAT);
  ALLOCATOR(GRALLOC1_PFN_SET_PRODUCER_USAGE, SET_PRODUCER_USAGE);
  ALLOCATOR(GRALLOC1_PFN_SET_CONSUMER_USAGE, SET_CONSUMER_USAGE);
  ALLOCATOR(GRALLOC1_PFN_ALLOCATE, ALLOCATE);
  ALLOCATOR(GRALLOC1_PFN_GET_STRIDE, GET_STRIDE);
#undef ALLOCATOR
  if (!GetFunction(state.allocator, GRALLOC1_FUNCTION_RELEASE, &state.release))
    return Fail("gralloc1 RELEASE");
  gralloc1_buffer_descriptor_t descriptor = 0;
  if (CREATE_DESCRIPTOR(state.allocator, &descriptor))
    return Fail("buffer descriptor");
  bool configured = SET_DIMENSIONS(state.allocator, descriptor, width, height) == 0 &&
      SET_FORMAT(state.allocator, descriptor, HAL_PIXEL_FORMAT_RGBA_8888) == 0 &&
      SET_PRODUCER_USAGE(state.allocator, descriptor, kProducerUsage) == 0 &&
      SET_CONSUMER_USAGE(state.allocator, descriptor, kConsumerUsage) == 0;
  for (unsigned i = 0; configured && i < state.buffer_count; ++i) {
    configured = ALLOCATE(state.allocator, 1, &descriptor, &state.buffers[i]) == 0 && state.buffers[i];
    uint32_t stride = 0;
    configured = configured && GET_STRIDE(state.allocator, state.buffers[i], &stride) == 0 &&
        stride >= static_cast<uint32_t>(width);
    auto& native = state.native[i];
    native.width = width;
    native.height = height;
    native.stride = stride;
    native.format = HAL_PIXEL_FORMAT_RGBA_8888;
    native.layerCount = 1;
    native.handle = state.buffers[i];
    native.usage = kNativeUsage;
    native.usage_deprecated = static_cast<int>(native.usage);
    native.common.incRef = Impl::Reference;
    native.common.decRef = Impl::Reference;
  }
  DESTROY_DESCRIPTOR(state.allocator, descriptor);
  if (!configured) return Fail("scanout allocation");

#define COMPOSER(type, name) \
  type name = nullptr; \
  if (!GetFunction(state.composer, HWC2_FUNCTION_##name, &name)) \
    return Fail("HWC2 " #name)
  COMPOSER(HWC2_PFN_CREATE_LAYER, CREATE_LAYER);
  COMPOSER(HWC2_PFN_SET_POWER_MODE, SET_POWER_MODE);
  COMPOSER(HWC2_PFN_SET_LAYER_COMPOSITION_TYPE, SET_LAYER_COMPOSITION_TYPE);
  COMPOSER(HWC2_PFN_SET_LAYER_DISPLAY_FRAME, SET_LAYER_DISPLAY_FRAME);
  COMPOSER(HWC2_PFN_SET_LAYER_SOURCE_CROP, SET_LAYER_SOURCE_CROP);
  COMPOSER(HWC2_PFN_SET_LAYER_BLEND_MODE, SET_LAYER_BLEND_MODE);
  COMPOSER(HWC2_PFN_SET_LAYER_PLANE_ALPHA, SET_LAYER_PLANE_ALPHA);
#undef COMPOSER
  if (!GetFunction(state.composer, HWC2_FUNCTION_REGISTER_CALLBACK, &state.register_callback) ||
      !GetFunction(state.composer, HWC2_FUNCTION_SET_CLIENT_TARGET, &state.set_target) ||
      !GetFunction(state.composer, HWC2_FUNCTION_VALIDATE_DISPLAY, &state.validate) ||
      !GetFunction(state.composer, HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES, &state.accept) ||
      !GetFunction(state.composer, HWC2_FUNCTION_PRESENT_DISPLAY, &state.present) ||
      !GetFunction(state.composer, HWC2_FUNCTION_GET_RELEASE_FENCES, &state.get_release_fences) ||
      !GetFunction(state.composer, HWC2_FUNCTION_DESTROY_LAYER, &state.destroy_layer))
    return Fail("HWC2 function table");
  if (state.register_callback(state.composer, HWC2_CALLBACK_HOTPLUG, &state,
          reinterpret_cast<hwc2_function_pointer_t>(Impl::Hotplug)))
    return Fail("hotplug registration");
  state.registered = true;
  if (!state.connected.load()) return Fail("primary display discovery");
  const auto display = state.display.load();
  if (SET_POWER_MODE(state.composer, display, HWC2_POWER_MODE_ON))
    return Fail("display power");
  if (CREATE_LAYER(state.composer, display, &state.layer))
    return Fail("client layer creation");
  state.has_layer = true;
  const hwc_rect_t frame{0, 0, width, height};
  const hwc_frect_t crop{0, 0, static_cast<float>(width), static_cast<float>(height)};
  state.ready = SET_LAYER_COMPOSITION_TYPE(state.composer, display, state.layer, HWC2_COMPOSITION_CLIENT) == 0 &&
      SET_LAYER_DISPLAY_FRAME(state.composer, display, state.layer, frame) == 0 &&
      SET_LAYER_SOURCE_CROP(state.composer, display, state.layer, crop) == 0 &&
      SET_LAYER_BLEND_MODE(state.composer, display, state.layer, HWC2_BLEND_MODE_NONE) == 0 &&
      SET_LAYER_PLANE_ALPHA(state.composer, display, state.layer, 1.0f) == 0;
  return state.ready || Fail("client layer setup");
}

void* MaliDisplay::Buffer(unsigned index) {
  return impl_->ready && index < impl_->buffer_count ? &impl_->native[index] : nullptr;
}

unsigned MaliDisplay::BufferCount() const { return impl_->buffer_count; }

bool MaliDisplay::Wait(unsigned index) {
  auto& state = *impl_;
  if (!state.ready || !state.connected.load() || index >= state.buffer_count) return false;
  if (state.fences[index] < 0) return true;
  pollfd fence{state.fences[index], POLLIN, 0};
  int result;
  do { result = poll(&fence, 1, 1000); } while (result < 0 && errno == EINTR);
  if (result <= 0 || !(fence.revents & POLLIN) ||
      (fence.revents & (POLLERR | POLLNVAL))) return Fail("scanout release fence");
  close(state.fences[index]);
  state.fences[index] = -1;
  return true;
}

bool MaliDisplay::Present(unsigned index, int acquire_fence) {
  auto& state = *impl_;
  if (!state.ready || !state.connected.load() || index >= state.buffer_count ||
      state.last_buffer == static_cast<int>(index)) {
    if (acquire_fence >= 0) close(acquire_fence);
    return false;
  }
  const auto display = state.display.load();
  const hwc_rect_t frame{0, 0, state.width, state.height};
  const hwc_region_t damage{1, &frame};
  const int target_result = state.set_target(state.composer, display,
      state.buffers[index], acquire_fence, 0, damage);
  // Some legacy composers consume the descriptor rather than duplicating it.
  if (acquire_fence >= 0 && !state.composer_owns_acquire_fence)
    close(acquire_fence);
  if (target_result)
    return Fail("client target");
  uint32_t types = 0, requests = 0;
  int result = state.validate(state.composer, display, &types, &requests);
  if (types != 0 || requests != 0) return Fail("unexpected client layer changes");
  if (result == HWC2_ERROR_HAS_CHANGES) result = state.accept(state.composer, display);
  if (result) return Fail("display validation");
  int fence = -1;
  result = state.present(state.composer, display, &fence);
  if (result) {
    if (fence >= 0) close(fence);
    return Fail("display presentation");
  }
  // Once the new target is displayed, the previous target can be reused.
  if (state.last_buffer >= 0) {
    auto& previous_fence = state.fences[state.last_buffer];
    if (previous_fence >= 0) close(previous_fence);
    previous_fence = fence;
  } else if (fence >= 0) {
    close(fence);
  }
  state.last_buffer = static_cast<int>(index);
  return state.ReleaseLayerFences();
}

}  // namespace aeraui

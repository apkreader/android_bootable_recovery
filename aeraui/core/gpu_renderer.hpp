/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <linux/types.h>

#include <lvgl.h>
#include <minuitwrp/minui.h>

struct AHardwareBuffer;

namespace aeraui {
class MaliDisplay;

// Loads vendor EGL at runtime and imports double-buffered scanout targets.
// Failed GPU setup leaves the software renderer available.
class GpuRenderer final {
 public:
  GpuRenderer() = default;
  ~GpuRenderer();

  GpuRenderer(const GpuRenderer&) = delete;
  GpuRenderer& operator=(const GpuRenderer&) = delete;

  bool Initialize(int32_t width, int32_t height);
  lv_display_t* CreateDisplay(int32_t logical_width, int32_t logical_height);
  bool Present(lv_display_t* display);
  void Shutdown();
  bool IsReady() const { return ready_; }

 private:
  void* egl_library_ = nullptr;
  void* gles_library_ = nullptr;
  void* nativewindow_library_ = nullptr;
  void* egl_display_ = nullptr;
  void* egl_surface_ = nullptr;
  void* egl_context_ = nullptr;
  void* egl_images_[3] = {};
  unsigned int textures_[3] = {};
  unsigned int framebuffers_[3] = {};
  AHardwareBuffer* hardware_buffers_[3] = {};
  gr_surface scanouts_[3] = {};
  unsigned buffer_count_ = 2;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t logical_width_ = 0;
  int32_t logical_height_ = 0;
  unsigned int next_buffer_ = 0;
  bool driver_initialized_ = false;
  bool ready_ = false;
  MaliDisplay* mali_display_ = nullptr;
  int (*duplicate_native_fence_)(void*, void*) = nullptr;
};

}  // namespace aeraui

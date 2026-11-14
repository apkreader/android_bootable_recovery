/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <memory>

namespace aeraui {

// Legacy Mali EGL with gralloc1 buffers and HWC2 client composition.
class MaliDisplay final {
 public:
  MaliDisplay();
  ~MaliDisplay();
  MaliDisplay(const MaliDisplay&) = delete;
  MaliDisplay& operator=(const MaliDisplay&) = delete;

  void* OpenDriver();
  bool Initialize(int width, int height);
  unsigned BufferCount() const;
  void* Buffer(unsigned index);
  bool Wait(unsigned index);
  // Takes ownership of acquire_fence, including on failure.
  bool Present(unsigned index, int acquire_fence = -1);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aeraui

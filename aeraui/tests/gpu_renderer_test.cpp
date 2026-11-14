/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../core/gpu_renderer.hpp"
#include "../core/mali_display.hpp"
#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>
#include <dlfcn.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <string>
#include "drivers/opengles/glad/include/glad/egl.h"
#include "drivers/opengles/glad/include/glad/gles2.h"
#include "drivers/opengles/lv_opengles_texture.h"
#include "drivers/opengles/lv_opengles_driver.h"
#include "drivers/opengles/lv_opengles_texture_private.h"

struct AHardwareBuffer { AHardwareBuffer_Desc description; };

namespace {
std::string mali_path;
bool fail_library, fail_mali, fail_framebuffer, fail_display;
bool native_fences, fail_sync;
int libraries, allocations, releases, imports, released_scanouts;
int images, contexts, surfaces, displays, textures, framebuffers;
int adreno_loads, mali_presents, drm_presents, finishes, draws, lvgl_shutdowns;
int flushes, clears;
int syncs, last_wait_flush;
unsigned texture_id = 10;
AHardwareBuffer buffers[3];
unsigned mali_buffer_count = 2;
alignas(native_handle_t) unsigned char handle_storage[sizeof(native_handle_t) + sizeof(int)];

void Reset() {
  assert(!libraries && !images && !contexts && !surfaces && !displays);
  assert(!textures && !framebuffers && allocations == releases && imports == released_scanouts);
  mali_path.clear();
  fail_library = fail_mali = fail_framebuffer = fail_display = false;
  allocations = releases = imports = released_scanouts = 0;
  adreno_loads = mali_presents = drm_presents = finishes = draws = lvgl_shutdowns = 0;
  flushes = clears = 0;
  native_fences = fail_sync = false;
  syncs = last_wait_flush = 0;
  mali_buffer_count = 2;
}
EGLDisplay EGLAPIENTRY GetDisplay(EGLNativeDisplayType) { return reinterpret_cast<EGLDisplay>(1); }
EGLBoolean EGLAPIENTRY Initialize(EGLDisplay, EGLint* major, EGLint* minor) {
  ++displays; *major = 1; *minor = 4; return EGL_TRUE;
}
EGLBoolean EGLAPIENTRY Terminate(EGLDisplay) { --displays; return EGL_TRUE; }
__eglMustCastToProperFunctionPointerType EGLAPIENTRY GetProcAddress(const char*) { return nullptr; }
EGLBoolean EGLAPIENTRY BindApi(EGLenum) { return EGL_TRUE; }
const char* EGLAPIENTRY QueryString(EGLDisplay, EGLint) {
  return "EGL_ANDROID_image_native_buffer EGL_ANDROID_native_fence_sync";
}
EGLBoolean EGLAPIENTRY ChooseConfig(EGLDisplay, const EGLint*, EGLConfig* out, EGLint, EGLint* count) {
  *out = reinterpret_cast<EGLConfig>(1); *count = 1; return EGL_TRUE;
}
EGLSurface EGLAPIENTRY CreateSurface(EGLDisplay, EGLConfig, const EGLint*) {
  ++surfaces; return reinterpret_cast<EGLSurface>(1);
}
EGLContext EGLAPIENTRY CreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*) {
  ++contexts; return reinterpret_cast<EGLContext>(1);
}
EGLBoolean EGLAPIENTRY MakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) { return EGL_TRUE; }
EGLBoolean EGLAPIENTRY DestroyContext(EGLDisplay, EGLContext) { --contexts; return EGL_TRUE; }
EGLBoolean EGLAPIENTRY DestroySurface(EGLDisplay, EGLSurface) { --surfaces; return EGL_TRUE; }
EGLint EGLAPIENTRY EglError() { return EGL_SUCCESS; }
EGLImageKHR EGLAPIENTRY CreateImage(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer buffer, const EGLint*) {
  assert(buffer); ++images; return reinterpret_cast<EGLImageKHR>(images);
}
EGLBoolean EGLAPIENTRY DestroyImage(EGLDisplay, EGLImageKHR) { --images; return EGL_TRUE; }
EGLSyncKHR EGLAPIENTRY CreateSync(EGLDisplay, EGLenum, const EGLint*) {
  if (fail_sync) return EGL_NO_SYNC_KHR;
  ++syncs; return reinterpret_cast<EGLSyncKHR>(1);
}
EGLBoolean EGLAPIENTRY DestroySync(EGLDisplay, EGLSyncKHR) { --syncs; return EGL_TRUE; }
int DuplicateFence(void*, void*) { return eventfd(0, EFD_CLOEXEC); }
const GLubyte* GLAD_API_PTR GetString(GLenum name) {
  return reinterpret_cast<const GLubyte*>(name == GL_EXTENSIONS ? "GL_OES_EGL_image" : "test GPU");
}
void GLAD_API_PTR GenerateTextures(GLsizei count, GLuint* out) {
  textures += count; for (int i = 0; i < count; ++i) out[i] = texture_id++;
}
void GLAD_API_PTR DeleteTextures(GLsizei count, const GLuint* ids) {
  for (int i = 0; i < count; ++i) if (ids[i]) --textures;
}
void GLAD_API_PTR GenerateFramebuffers(GLsizei count, GLuint* out) {
  framebuffers += count; for (int i = 0; i < count; ++i) out[i] = texture_id++;
}
void GLAD_API_PTR DeleteFramebuffers(GLsizei count, const GLuint* ids) {
  for (int i = 0; i < count; ++i) if (ids[i]) --framebuffers;
}
void GLAD_API_PTR BindTexture(GLenum, GLuint) {}
void GLAD_API_PTR TextureParameter(GLenum, GLenum, GLint) {}
void GLAD_API_PTR BindFramebuffer(GLenum, GLuint) {}
void GLAD_API_PTR FramebufferTexture(GLenum, GLenum, GLenum, GLuint, GLint) {}
GLenum GLAD_API_PTR FramebufferStatus(GLenum) { return fail_framebuffer ? 0 : GL_FRAMEBUFFER_COMPLETE; }
void GLAD_API_PTR Viewport(GLint, GLint, GLsizei, GLsizei) {}
void GLAD_API_PTR Disable(GLenum) {}
void GLAD_API_PTR ClearColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
void GLAD_API_PTR Clear(GLbitfield) { ++clears; }
void GLAD_API_PTR Flush() { ++flushes; }
void GLAD_API_PTR Finish() { ++finishes; }
GLenum GLAD_API_PTR GlError() { return GL_NO_ERROR; }
void ImageTarget(GLenum, void*) {}
int Allocate(const AHardwareBuffer_Desc* description, AHardwareBuffer** out) {
  assert(allocations < 2);
  *out = &buffers[allocations++]; (*out)->description = *description;
  (*out)->description.stride = description->width;
  return 0;
}
void Release(AHardwareBuffer*) { ++releases; }
void Describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* out) { *out = buffer->description; }
const native_handle_t* Handle(const AHardwareBuffer*) {
  auto* handle = reinterpret_cast<native_handle_t*>(handle_storage);
  handle->numFds = 1;
  handle->data[0] = 42;
  return handle;
}
void* NativeBuffer(const AHardwareBuffer* buffer) { return const_cast<AHardwareBuffer*>(buffer); }
int Import(const GRDrmBufferInfo* info, gr_surface* out) {
  assert(info->width == 1080 && info->height == 2312 && info->pitch == 4320);
  *out = reinterpret_cast<gr_surface>(++imports); return 0;
}
int ReleaseScanout(gr_surface) { ++released_scanouts; return 0; }
}  // namespace

namespace android::base {
std::string GetProperty(const std::string& key, const std::string& fallback) {
  if (key == "ro.aera.gpu.mali_driver") return mali_path;
  if (key == "ro.aera.gpu.native_fence") return native_fences ? "true" : "false";
  return fallback;
}
}
namespace aeraui {
struct MaliDisplay::Impl {};
MaliDisplay::MaliDisplay() : impl_(new Impl) {}
MaliDisplay::~MaliDisplay() = default;
void* MaliDisplay::OpenDriver() { return fail_mali ? nullptr : reinterpret_cast<void*>(4); }
bool MaliDisplay::Initialize(int, int) { return !fail_mali; }
unsigned MaliDisplay::BufferCount() const { return mali_buffer_count; }
void* MaliDisplay::Buffer(unsigned index) { return &buffers[index]; }
bool MaliDisplay::Wait(unsigned) {
  assert(flushes > last_wait_flush && clears == mali_presents);
  last_wait_flush = flushes;
  return true;
}
bool MaliDisplay::Present(unsigned, int acquire_fence) {
  if (native_fences && !fail_sync) assert(acquire_fence >= 0 && finishes == 0);
  else assert(acquire_fence < 0 && finishes > 0);
  if (acquire_fence >= 0) close(acquire_fence);
  ++mali_presents;
  return true;
}
}

extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" void* __wrap_dlopen(const char* path, int) {
  if (fail_library) return nullptr;
  if (std::strstr(path, "adreno")) ++adreno_loads;
  ++libraries;
  return reinterpret_cast<void*>(1);
}
extern "C" int __wrap_dlclose(void*) { --libraries; return 0; }
extern "C" void* __wrap_dlsym(void*, const char* name) {
#define SYMBOL(symbol, implementation) if (!std::strcmp(name, symbol)) return reinterpret_cast<void*>(implementation)
  SYMBOL("eglGetDisplay", GetDisplay);
  SYMBOL("eglInitialize", Initialize);
  SYMBOL("eglTerminate", Terminate);
  SYMBOL("eglGetProcAddress", GetProcAddress);
  SYMBOL("eglDupNativeFenceFDANDROID", DuplicateFence);
  SYMBOL("glEGLImageTargetTexture2DOES", ImageTarget);
  SYMBOL("AHardwareBuffer_allocate", Allocate);
  SYMBOL("AHardwareBuffer_release", Release);
  SYMBOL("AHardwareBuffer_describe", Describe);
  SYMBOL("AHardwareBuffer_getNativeHandle", Handle);
  SYMBOL("_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEPK15AHardwareBuffer", NativeBuffer);
  SYMBOL("gr_drm_import_scanout_buffer", Import);
  SYMBOL("gr_drm_release_imported_scanout_buffer", ReleaseScanout);
#undef SYMBOL
  return nullptr;
}
extern "C" int __wrap_gladLoadEGLUserPtr(EGLDisplay, GLADuserptrloadfunc, void*) {
  glad_eglBindAPI = BindApi; glad_eglQueryString = QueryString;
  glad_eglChooseConfig = ChooseConfig; glad_eglCreatePbufferSurface = CreateSurface;
  glad_eglCreateContext = CreateContext; glad_eglMakeCurrent = MakeCurrent;
  glad_eglGetError = EglError; glad_eglCreateImageKHR = CreateImage;
  glad_eglDestroyImageKHR = DestroyImage; glad_eglDestroyContext = DestroyContext;
  glad_eglDestroySurface = DestroySurface; glad_eglTerminate = Terminate;
  glad_eglCreateSyncKHR = CreateSync; glad_eglDestroySyncKHR = DestroySync;
  return 1;
}
extern "C" int __wrap_gladLoadGLES2UserPtr(GLADuserptrloadfunc, void*) {
  glad_glGetString = GetString; glad_glGenTextures = GenerateTextures;
  glad_glDeleteTextures = DeleteTextures; glad_glBindTexture = BindTexture;
  glad_glTexParameteri = TextureParameter; glad_glGenFramebuffers = GenerateFramebuffers;
  glad_glDeleteFramebuffers = DeleteFramebuffers; glad_glBindFramebuffer = BindFramebuffer;
  glad_glFramebufferTexture2D = FramebufferTexture; glad_glCheckFramebufferStatus = FramebufferStatus;
  glad_glViewport = Viewport; glad_glDisable = Disable; glad_glClearColor = ClearColor;
  glad_glClear = Clear; glad_glFinish = Finish; glad_glFlush = Flush; glad_glGetError = GlError;
  return 1;
}
extern "C" {
lv_display_t* lv_opengles_texture_create(int32_t, int32_t) {
  return fail_display ? nullptr : reinterpret_cast<lv_display_t*>(1);
}
GLuint lv_opengles_texture_get_texture_id(lv_display_t*) { return 1; }
lv_result_t lv_opengles_texture_reshape(lv_opengles_texture_t*, lv_display_t*, int32_t, int32_t) { return LV_RESULT_OK; }
void lv_opengles_deinit() { ++lvgl_shutdowns; }
void lv_opengles_render_display_texture(lv_display_t*, bool flip_x, bool flip_y) {
  assert(!flip_x && !flip_y); ++draws;
}
void* lv_display_get_driver_data(lv_display_t*) { return nullptr; }
int32_t lv_display_get_horizontal_resolution(const lv_display_t*) { return 1440; }
int32_t lv_display_get_vertical_resolution(const lv_display_t*) { return 3082; }
void lv_display_add_event_cb(lv_display_t*, lv_event_cb_t, lv_event_code_t, void*) {}
void* lv_event_get_target(lv_event_t*) { return nullptr; }
}
int gr_drm_present(gr_surface) { ++drm_presents; return 0; }

int main() {
  Reset();
  {
    aeraui::GpuRenderer renderer;
    assert(!renderer.Initialize(0, 2312));
    fail_library = true;
    assert(!renderer.Initialize(1080, 2312) && !renderer.IsReady());
  }
  Reset();
  {
    aeraui::GpuRenderer renderer;
    mali_path = "/test/mali.so"; fail_mali = true;
    assert(!renderer.Initialize(1080, 2312) && !renderer.IsReady());
    assert(adreno_loads == 0);
  }
  Reset();
  {
    aeraui::GpuRenderer renderer;
    fail_framebuffer = true;
    assert(!renderer.Initialize(1080, 2312));
    assert(!renderer.IsReady() && allocations == releases && imports == released_scanouts);
  }
  Reset();
  {
    aeraui::GpuRenderer renderer;
    native_fences = true;
    assert(renderer.Initialize(1080, 2312) && renderer.IsReady());
    assert(adreno_loads == 2 && allocations == 2 && imports == 2);
    auto* display = renderer.CreateDisplay(1440, 3082); assert(display);
    assert(renderer.Present(display) && renderer.Present(display));
    assert(drm_presents == 2 && mali_presents == 0 && draws == 2);
    assert(flushes == 0);
    renderer.Shutdown(); renderer.Shutdown();
    assert(lvgl_shutdowns == 1);
  }
  Reset();
  {
    aeraui::GpuRenderer renderer;
    mali_path = "/test/mali.so";
    assert(renderer.Initialize(1080, 2312));
    assert(adreno_loads == 0 && allocations == 0 && imports == 0);
    auto* display = renderer.CreateDisplay(1440, 3082); assert(display);
    assert(renderer.Present(display) && renderer.Present(display));
    assert(mali_presents == 2 && drm_presents == 0);
    assert(flushes == 2);
  }
  Reset();
  {
    aeraui::GpuRenderer renderer;
    assert(renderer.Initialize(1080, 2312));
    fail_display = true;
    assert(!renderer.CreateDisplay(1440, 3082));
    renderer.Shutdown();
    assert(lvgl_shutdowns == 0);
  }
  Reset();
  for (bool sync_failure : {false, true}) {
    {
      aeraui::GpuRenderer renderer;
      mali_path = "/test/mali.so";
      native_fences = true;
      fail_sync = sync_failure;
      assert(renderer.Initialize(1080, 2312));
      auto* display = renderer.CreateDisplay(1440, 3082); assert(display);
      assert(renderer.Present(display) && renderer.Present(display));
      assert(finishes == (sync_failure ? 2 : 0));
      assert(syncs == 0 && mali_presents == 2);
    }
    Reset();
  }
  {
    aeraui::GpuRenderer renderer;
    mali_path = "/test/mali.so";
    mali_buffer_count = 3;
    assert(renderer.Initialize(1080, 2312));
    assert(images == 3 && textures == 3 && framebuffers == 3);
    auto* display = renderer.CreateDisplay(1440, 3082); assert(display);
    for (int i = 0; i < 5; ++i) assert(renderer.Present(display));
    assert(mali_presents == 5 && drm_presents == 0);
  }
  Reset();
}

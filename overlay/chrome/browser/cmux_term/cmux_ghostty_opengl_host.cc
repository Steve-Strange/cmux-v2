// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_ghostty_opengl_host.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"

#if BUILDFLAG(IS_LINUX)
#include <dlfcn.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#elif BUILDFLAG(IS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>
#else
#error "The host-owned OpenGL adapter is only built on Linux and Windows"
#endif

namespace cmux {

namespace {

constexpr unsigned int kGlPackAlignment = 0x0D05;
constexpr unsigned int kGlRgba = 0x1908;
constexpr unsigned int kGlUnsignedByte = 0x1401;
constexpr uint32_t kMaxDrawableDimension = 16384;
constexpr uint64_t kMaxFrameBytes = 512ull * 1024ull * 1024ull;

uint32_t ClampDimension(uint32_t value) {
  return std::clamp(value, 1u, kMaxDrawableDimension);
}

#if BUILDFLAG(IS_LINUX)

class LinuxOpenGLHost final : public CmuxGhosttyOpenGLHost {
 public:
  LinuxOpenGLHost(uint32_t width,
                  uint32_t height,
                  FrameCallback frame_callback)
      : CmuxGhosttyOpenGLHost(std::move(frame_callback)),
        requested_width_(ClampDimension(width)),
        requested_height_(ClampDimension(height)) {}

  ~LinuxOpenGLHost() override {
    if (display_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
      }
      if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
      }
      eglTerminate(display_);
    }
    if (lib_gl_) {
      dlclose(lib_gl_);
    }
  }

  bool Initialize() {
    lib_gl_ = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);

    // Prefer Mesa's display-independent surfaceless platform. Fall back to the
    // default EGL display for proprietary drivers without the extension.
#if defined(EGL_PLATFORM_SURFACELESS_MESA)
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display) {
      display_ = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                      reinterpret_cast<void*>(
                                          EGL_DEFAULT_DISPLAY),
                                      nullptr);
      if (display_ != EGL_NO_DISPLAY &&
          eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
        display_ = EGL_NO_DISPLAY;
      }
    }
#endif
    if (display_ == EGL_NO_DISPLAY) {
      display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
      if (display_ == EGL_NO_DISPLAY ||
          eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
        LOG(ERROR) << "cmux-term: unable to initialize an EGL display";
        display_ = EGL_NO_DISPLAY;
        return false;
      }
    }
    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
      LOG(ERROR) << "cmux-term: EGL cannot bind the desktop OpenGL API";
      return false;
    }

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,     8,               EGL_GREEN_SIZE,     8,
        EGL_BLUE_SIZE,    8,               EGL_ALPHA_SIZE,     8,
        EGL_DEPTH_SIZE,   24,              EGL_STENCIL_SIZE,   8,
        EGL_NONE,
    };
    EGLint config_count = 0;
    if (eglChooseConfig(display_, config_attributes, &config_, 1,
                        &config_count) != EGL_TRUE ||
        config_count == 0) {
      LOG(ERROR) << "cmux-term: no OpenGL EGL pbuffer configuration";
      return false;
    }

    const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR,
        4,
        EGL_CONTEXT_MINOR_VERSION_KHR,
        3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_NONE,
    };
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT,
                                context_attributes);
    if (context_ == EGL_NO_CONTEXT) {
      const EGLint fallback_attributes[] = {EGL_NONE};
      context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT,
                                  fallback_attributes);
    }
    if (context_ == EGL_NO_CONTEXT) {
      LOG(ERROR) << "cmux-term: unable to create an OpenGL 4.3 EGL context";
      return false;
    }
    current_width_ = requested_width_.load(std::memory_order_relaxed);
    current_height_ = requested_height_.load(std::memory_order_relaxed);
    surface_ = CreateSurface(current_width_, current_height_);
    if (surface_ == EGL_NO_SURFACE) {
      LOG(ERROR) << "cmux-term: unable to create an EGL pbuffer";
      return false;
    }
    return true;
  }

  void SetSize(uint32_t width, uint32_t height) override {
    requested_width_.store(ClampDimension(width), std::memory_order_release);
    requested_height_.store(ClampDimension(height), std::memory_order_release);
  }

 private:
  using GlPixelStorei = void (*)(unsigned int, int);
  using GlReadPixels = void (*)(int,
                                int,
                                int,
                                int,
                                unsigned int,
                                unsigned int,
                                void*);

  EGLSurface CreateSurface(uint32_t width, uint32_t height) {
    const EGLint attributes[] = {
        EGL_WIDTH, static_cast<EGLint>(width), EGL_HEIGHT,
        static_cast<EGLint>(height), EGL_NONE,
    };
    return eglCreatePbufferSurface(display_, config_, attributes);
  }

  bool MakeCurrent() override {
    return display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE &&
           context_ != EGL_NO_CONTEXT &&
           eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
  }

  void ClearCurrent() override {
    if (display_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
  }

  void* GetProcAddress(const char* name) override {
    if (!name) {
      return nullptr;
    }
    void* proc = reinterpret_cast<void*>(eglGetProcAddress(name));
    if (!proc && lib_gl_) {
      proc = dlsym(lib_gl_, name);
    }
    return proc;
  }

  void SwapBuffers() override {
    auto pixel_store_i =
        reinterpret_cast<GlPixelStorei>(GetProcAddress("glPixelStorei"));
    auto read_pixels =
        reinterpret_cast<GlReadPixels>(GetProcAddress("glReadPixels"));
    EmitFrame(current_width_, current_height_,
              reinterpret_cast<void*>(pixel_store_i),
              reinterpret_cast<void*>(read_pixels));
    eglSwapBuffers(display_, surface_);
    ApplyPendingSize();
  }

  void ApplyPendingSize() {
    const uint32_t width = requested_width_.load(std::memory_order_acquire);
    const uint32_t height = requested_height_.load(std::memory_order_acquire);
    if ((width == current_width_ && height == current_height_) ||
        display_ == EGL_NO_DISPLAY) {
      return;
    }
    EGLSurface replacement = CreateSurface(width, height);
    if (replacement == EGL_NO_SURFACE) {
      LOG(ERROR) << "cmux-term: failed to resize the EGL pbuffer";
      return;
    }
    if (eglMakeCurrent(display_, replacement, replacement, context_) !=
        EGL_TRUE) {
      eglDestroySurface(display_, replacement);
      LOG(ERROR) << "cmux-term: failed to activate the resized EGL pbuffer";
      return;
    }
    EGLSurface old_surface = surface_;
    surface_ = replacement;
    current_width_ = width;
    current_height_ = height;
    eglDestroySurface(display_, old_surface);
  }

  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLConfig config_ = nullptr;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  raw_ptr<void> lib_gl_ = nullptr;
  std::atomic<uint32_t> requested_width_;
  std::atomic<uint32_t> requested_height_;
  uint32_t current_width_ = 1;
  uint32_t current_height_ = 1;
};

#elif BUILDFLAG(IS_WIN)

constexpr int kWglContextMajorVersionArb = 0x2091;
constexpr int kWglContextMinorVersionArb = 0x2092;
constexpr int kWglContextProfileMaskArb = 0x9126;
constexpr int kWglContextCoreProfileBitArb = 0x00000001;
constexpr int kWglContextCompatibilityProfileBitArb = 0x00000002;
constexpr wchar_t kHiddenOpenGLWindowClass[] =
    L"CmuxGhosttyHiddenOpenGLWindow";

using WglCreateContextAttribsArb = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using WglChoosePixelFormatFn = int(WINAPI*)(HDC,
                                            const PIXELFORMATDESCRIPTOR*);
using WglCreateContextFn = HGLRC(WINAPI*)(HDC);
using WglDeleteContextFn = BOOL(WINAPI*)(HGLRC);
using WglGetCurrentContextFn = HGLRC(WINAPI*)();
using WglGetProcAddressFn = PROC(WINAPI*)(LPCSTR);
using WglMakeCurrentFn = BOOL(WINAPI*)(HDC, HGLRC);
using WglSetPixelFormatFn = BOOL(WINAPI*)(HDC,
                                         int,
                                         const PIXELFORMATDESCRIPTOR*);
using WglSwapBuffersFn = BOOL(WINAPI*)(HDC);

bool EnsureHiddenOpenGLWindowClass() {
  static const bool registered = [] {
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kHiddenOpenGLWindowClass;
    return RegisterClassExW(&window_class) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return registered;
}

class WindowsOpenGLHost final : public CmuxGhosttyOpenGLHost {
 public:
  WindowsOpenGLHost(uint32_t width,
                    uint32_t height,
                    FrameCallback frame_callback)
      : CmuxGhosttyOpenGLHost(std::move(frame_callback)),
        width_(ClampDimension(width)),
        height_(ClampDimension(height)) {}

  ~WindowsOpenGLHost() override {
    if (render_context_ && wgl_get_current_context_ && wgl_make_current_ &&
        wgl_get_current_context_() == render_context_) {
      wgl_make_current_(nullptr, nullptr);
    }
    if (render_context_ && wgl_delete_context_) {
      wgl_delete_context_(render_context_);
    }
    if (device_context_ && window_) {
      ReleaseDC(window_, device_context_);
    }
    if (window_) {
      DestroyWindow(window_);
    }
    if (opengl_module_) {
      FreeLibrary(opengl_module_);
    }
  }

  bool Initialize() {
    wchar_t override_path[MAX_PATH] = {};
    const DWORD override_length = GetEnvironmentVariableW(
        L"GHOSTTY_MESA_OPENGL_PATH", override_path, MAX_PATH);
    const bool override_opengl =
        override_length > 0 && override_length < MAX_PATH;
    opengl_module_ =
        override_opengl
            ? LoadLibraryExW(override_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)
            : LoadLibraryW(L"opengl32.dll");
    if (!opengl_module_) {
      LOG(ERROR) << "cmux-term: unable to load an OpenGL implementation";
      return false;
    }
    const auto load = [this](const char* name) {
      return ::GetProcAddress(opengl_module_, name);
    };
    wgl_create_context_ =
        reinterpret_cast<WglCreateContextFn>(load("wglCreateContext"));
    wgl_delete_context_ =
        reinterpret_cast<WglDeleteContextFn>(load("wglDeleteContext"));
    wgl_get_current_context_ = reinterpret_cast<WglGetCurrentContextFn>(
        load("wglGetCurrentContext"));
    wgl_get_proc_address_ =
        reinterpret_cast<WglGetProcAddressFn>(load("wglGetProcAddress"));
    wgl_make_current_ =
        reinterpret_cast<WglMakeCurrentFn>(load("wglMakeCurrent"));
    if (override_opengl) {
      choose_pixel_format_ = reinterpret_cast<WglChoosePixelFormatFn>(
          load("wglChoosePixelFormat"));
      set_pixel_format_ =
          reinterpret_cast<WglSetPixelFormatFn>(load("wglSetPixelFormat"));
      swap_buffers_ =
          reinterpret_cast<WglSwapBuffersFn>(load("wglSwapBuffers"));
    } else {
      choose_pixel_format_ = &::ChoosePixelFormat;
      set_pixel_format_ = &::SetPixelFormat;
      swap_buffers_ = &::SwapBuffers;
    }
    if (!wgl_create_context_ || !wgl_delete_context_ ||
        !wgl_get_current_context_ || !wgl_get_proc_address_ ||
        !wgl_make_current_ || !choose_pixel_format_ || !set_pixel_format_ ||
        !swap_buffers_) {
      LOG(ERROR) << "cmux-term: OpenGL implementation lacks required WGL API";
      return false;
    }

    if (!EnsureHiddenOpenGLWindowClass()) {
      LOG(ERROR) << "cmux-term: unable to register the hidden WGL class";
      return false;
    }
    window_ = CreateWindowExW(0, kHiddenOpenGLWindowClass,
                              L"cmux Ghostty OpenGL",
                              WS_POPUP | WS_DISABLED, 0, 0,
                              static_cast<int>(width_),
                              static_cast<int>(height_), nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!window_) {
      LOG(ERROR) << "cmux-term: unable to create the hidden WGL window";
      return false;
    }
    device_context_ = GetDC(window_);
    if (!device_context_) {
      LOG(ERROR) << "cmux-term: GetDC failed for the hidden WGL window";
      return false;
    }

    PIXELFORMATDESCRIPTOR format = {};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags =
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cAlphaBits = 8;
    format.cDepthBits = 24;
    format.cStencilBits = 8;
    format.iLayerType = PFD_MAIN_PLANE;

    const int pixel_format = choose_pixel_format_(device_context_, &format);
    if (pixel_format == 0 ||
        !set_pixel_format_(device_context_, pixel_format, &format)) {
      LOG(ERROR) << "cmux-term: unable to set the WGL pixel format";
      return false;
    }
    HGLRC legacy = wgl_create_context_(device_context_);
    if (!legacy || !wgl_make_current_(device_context_, legacy)) {
      LOG(ERROR) << "cmux-term: unable to create the WGL bootstrap context";
      if (legacy) {
        wgl_delete_context_(legacy);
      }
      return false;
    }

    auto create_context = reinterpret_cast<WglCreateContextAttribsArb>(
        wgl_get_proc_address_("wglCreateContextAttribsARB"));
    HGLRC modern = nullptr;
    if (create_context) {
      const int profiles[] = {kWglContextCoreProfileBitArb,
                              kWglContextCompatibilityProfileBitArb};
      const int versions[][2] = {{4, 5}, {4, 3}};
      for (const auto& version : versions) {
        for (int profile : profiles) {
          const int attributes[] = {
              kWglContextMajorVersionArb, version[0],
              kWglContextMinorVersionArb, version[1],
              kWglContextProfileMaskArb, profile, 0,
          };
          modern = create_context(device_context_, nullptr, attributes);
          if (modern) {
            break;
          }
        }
        if (modern) {
          break;
        }
      }
    }
    if (modern) {
      wgl_make_current_(nullptr, nullptr);
      wgl_delete_context_(legacy);
      render_context_ = modern;
    } else {
      render_context_ = legacy;
    }
    wgl_make_current_(nullptr, nullptr);
    return true;
  }

  void SetSize(uint32_t width, uint32_t height) override {
    width_ = ClampDimension(width);
    height_ = ClampDimension(height);
    if (!SetWindowPos(window_, nullptr, 0, 0, static_cast<int>(width_),
                      static_cast<int>(height_),
                      SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER)) {
      LOG(ERROR) << "cmux-term: failed to resize the hidden WGL window";
    }
  }

 private:
  using GlPixelStorei = void(APIENTRY*)(unsigned int, int);
  using GlReadPixels = void(APIENTRY*)(int,
                                       int,
                                       int,
                                       int,
                                       unsigned int,
                                       unsigned int,
                                       void*);

  bool MakeCurrent() override {
    return device_context_ && render_context_ &&
           wgl_make_current_(device_context_, render_context_) == TRUE;
  }

  void ClearCurrent() override { wgl_make_current_(nullptr, nullptr); }

  void* GetProcAddress(const char* name) override {
    if (!name) {
      return nullptr;
    }
    PROC proc = wgl_get_proc_address_(name);
    if (proc && proc != reinterpret_cast<PROC>(1) &&
        proc != reinterpret_cast<PROC>(2) &&
        proc != reinterpret_cast<PROC>(3) &&
        proc != reinterpret_cast<PROC>(-1)) {
      return reinterpret_cast<void*>(proc);
    }
    return opengl_module_
               ? reinterpret_cast<void*>(::GetProcAddress(opengl_module_, name))
               : nullptr;
  }

  void SwapBuffers() override {
    auto pixel_store_i =
        reinterpret_cast<GlPixelStorei>(GetProcAddress("glPixelStorei"));
    auto read_pixels =
        reinterpret_cast<GlReadPixels>(GetProcAddress("glReadPixels"));
    RECT bounds = {};
    if (GetClientRect(window_, &bounds)) {
      EmitFrame(static_cast<uint32_t>(std::max(1L, bounds.right - bounds.left)),
                static_cast<uint32_t>(std::max(1L, bounds.bottom - bounds.top)),
                reinterpret_cast<void*>(pixel_store_i),
                reinterpret_cast<void*>(read_pixels));
    }
    if (!swap_buffers_(device_context_)) {
      LOG(ERROR) << "cmux-term: WGL SwapBuffers failed";
    }
  }

  HWND window_ = nullptr;
  HDC device_context_ = nullptr;
  HGLRC render_context_ = nullptr;
  HMODULE opengl_module_ = nullptr;
  WglCreateContextFn wgl_create_context_ = nullptr;
  WglDeleteContextFn wgl_delete_context_ = nullptr;
  WglGetCurrentContextFn wgl_get_current_context_ = nullptr;
  WglGetProcAddressFn wgl_get_proc_address_ = nullptr;
  WglMakeCurrentFn wgl_make_current_ = nullptr;
  WglChoosePixelFormatFn choose_pixel_format_ = nullptr;
  WglSetPixelFormatFn set_pixel_format_ = nullptr;
  WglSwapBuffersFn swap_buffers_ = nullptr;
  uint32_t width_ = 1;
  uint32_t height_ = 1;
};

#endif

}  // namespace

CmuxGhosttyOpenGLHost::CmuxGhosttyOpenGLHost(FrameCallback frame_callback)
    : frame_callback_(std::move(frame_callback)) {}

CmuxGhosttyOpenGLHost::~CmuxGhosttyOpenGLHost() = default;

// static
std::unique_ptr<CmuxGhosttyOpenGLHost> CmuxGhosttyOpenGLHost::Create(
    uint32_t width,
    uint32_t height,
    FrameCallback frame_callback) {
#if BUILDFLAG(IS_LINUX)
  auto host = std::make_unique<LinuxOpenGLHost>(width, height,
                                                std::move(frame_callback));
#elif BUILDFLAG(IS_WIN)
  auto host = std::make_unique<WindowsOpenGLHost>(width, height,
                                                  std::move(frame_callback));
#endif
  if (!host->Initialize()) {
    return nullptr;
  }
  return host;
}

void CmuxGhosttyOpenGLHost::PopulateSurfaceConfig(
    ghostty_surface_config_s* config) {
  CHECK(config);
  config->platform_tag = GHOSTTY_PLATFORM_OPENGL;
  config->platform.opengl.userdata = this;
  config->platform.opengl.make_current = &MakeCurrentThunk;
  config->platform.opengl.clear_current = &ClearCurrentThunk;
  config->platform.opengl.get_proc_address = &GetProcAddressThunk;
  config->platform.opengl.swap_buffers = &SwapBuffersThunk;
}

void CmuxGhosttyOpenGLHost::EmitFrame(
    uint32_t width,
    uint32_t height,
    void* pixel_store_i_ptr,
    void* read_pixels_ptr) {
  using PixelStorei =
#if BUILDFLAG(IS_WIN)
      void(APIENTRY*)(unsigned int, int);
  using ReadPixels = void(APIENTRY*)(int,
#else
      void (*)(unsigned int, int);
  using ReadPixels = void (*)(int,
#endif
                              int,
                              int,
                              int,
                              unsigned int,
                              unsigned int,
                              void*);
  auto pixel_store_i = reinterpret_cast<PixelStorei>(pixel_store_i_ptr);
  auto read_pixels = reinterpret_cast<ReadPixels>(read_pixels_ptr);
  if (!frame_callback_ || !pixel_store_i || !read_pixels || width == 0 ||
      height == 0) {
    return;
  }
  const uint64_t byte_count =
      static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u;
  if (byte_count > kMaxFrameBytes ||
      byte_count > std::numeric_limits<size_t>::max()) {
    LOG(ERROR) << "cmux-term: refusing oversized OpenGL readback frame";
    return;
  }
  CmuxGhosttyOpenGLFrame frame;
  frame.width = width;
  frame.height = height;
  frame.stride = width * 4u;
  frame.pixels.resize(static_cast<size_t>(byte_count));
  pixel_store_i(kGlPackAlignment, 1);
  read_pixels(0, 0, static_cast<int>(width), static_cast<int>(height), kGlRgba,
              kGlUnsignedByte, frame.pixels.data());
  frame_callback_.Run(std::move(frame));
}

// static
bool CmuxGhosttyOpenGLHost::MakeCurrentThunk(void* userdata) {
  return userdata &&
         static_cast<CmuxGhosttyOpenGLHost*>(userdata)->MakeCurrent();
}

// static
void CmuxGhosttyOpenGLHost::ClearCurrentThunk(void* userdata) {
  if (userdata) {
    static_cast<CmuxGhosttyOpenGLHost*>(userdata)->ClearCurrent();
  }
}

// static
void* CmuxGhosttyOpenGLHost::GetProcAddressThunk(void* userdata,
                                                 const char* name) {
  return userdata
             ? static_cast<CmuxGhosttyOpenGLHost*>(userdata)->GetProcAddress(
                   name)
             : nullptr;
}

// static
void CmuxGhosttyOpenGLHost::SwapBuffersThunk(void* userdata) {
  if (userdata) {
    static_cast<CmuxGhosttyOpenGLHost*>(userdata)->SwapBuffers();
  }
}

}  // namespace cmux

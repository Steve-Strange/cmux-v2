// Standalone validation for manaflow Ghostty's host-owned OpenGL embedding ABI.
// It creates a surfaceless EGL pbuffer, feeds a manual-mirror terminal stream,
// and proves that Ghostty presents nonempty RGBA frames through swap_buffers.
//
// Example (inside a Linux environment with EGL/GL development packages):
//   cc -std=c11 -O2 -I/path/to/ghostty/include ghostty_opengl_harness.c \
//      -L/path/to/ghostty/lib -lghostty-internal -lEGL -lGL -ldl -lpthread \
//      -o ghostty-opengl-harness
//   EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
//      LD_LIBRARY_PATH=/path/to/ghostty/lib ./ghostty-opengl-harness

#define _POSIX_C_SOURCE 200809L

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ghostty.h"

enum {
  kWidth = 800,
  kHeight = 600,
};

typedef struct {
  EGLDisplay display;
  EGLContext context;
  EGLSurface surface;
  atomic_uint frames;
  atomic_ullong checksum;
} opengl_host_s;

static ghostty_app_t g_app;

static void wakeup_cb(void* userdata) {
  (void)userdata;
}

static bool action_cb(ghostty_app_t app,
                      ghostty_target_s target,
                      ghostty_action_s action) {
  (void)app;
  if (action.tag == GHOSTTY_ACTION_RENDER &&
      target.tag == GHOSTTY_TARGET_SURFACE) {
    ghostty_surface_draw(target.target.surface);
    return true;
  }
  return false;
}

static bool read_clipboard_cb(void* userdata,
                              ghostty_clipboard_e clipboard,
                              void* state) {
  (void)userdata;
  (void)clipboard;
  (void)state;
  return false;
}

static void confirm_read_clipboard_cb(void* userdata,
                                      const char* text,
                                      void* state,
                                      ghostty_clipboard_request_e request) {
  (void)userdata;
  (void)text;
  (void)state;
  (void)request;
}

static void write_clipboard_cb(void* userdata,
                               ghostty_clipboard_e clipboard,
                               const ghostty_clipboard_content_s* content,
                               size_t content_count,
                               bool confirm) {
  (void)userdata;
  (void)clipboard;
  (void)content;
  (void)content_count;
  (void)confirm;
}

static void close_surface_cb(void* userdata, bool process_alive) {
  (void)userdata;
  (void)process_alive;
}

static void io_write_cb(void* userdata, const char* bytes, uintptr_t length) {
  (void)userdata;
  (void)bytes;
  (void)length;
}

static bool make_current_cb(void* userdata) {
  opengl_host_s* host = userdata;
  return host &&
         eglMakeCurrent(host->display, host->surface, host->surface,
                        host->context) == EGL_TRUE;
}

static void clear_current_cb(void* userdata) {
  opengl_host_s* host = userdata;
  if (host) {
    eglMakeCurrent(host->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
  }
}

static void* get_proc_address_cb(void* userdata, const char* name) {
  (void)userdata;
  return (void*)eglGetProcAddress(name);
}

static void swap_buffers_cb(void* userdata) {
  opengl_host_s* host = userdata;
  if (!host) {
    return;
  }
  const size_t byte_count = (size_t)kWidth * kHeight * 4u;
  uint8_t* pixels = malloc(byte_count);
  if (!pixels) {
    return;
  }
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  uint64_t checksum = 1469598103934665603ull;
  for (size_t i = 0; i < byte_count; i += 97) {
    checksum ^= pixels[i];
    checksum *= 1099511628211ull;
  }
  free(pixels);
  atomic_store_explicit(&host->checksum, checksum, memory_order_release);
  atomic_fetch_add_explicit(&host->frames, 1, memory_order_release);
  eglSwapBuffers(host->display, host->surface);
}

static bool initialize_opengl(opengl_host_s* host) {
  memset(host, 0, sizeof(*host));
  host->display = EGL_NO_DISPLAY;
  host->context = EGL_NO_CONTEXT;
  host->surface = EGL_NO_SURFACE;

  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
          "eglGetPlatformDisplayEXT");
#ifdef EGL_PLATFORM_SURFACELESS_MESA
  if (get_platform_display) {
    host->display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                         EGL_DEFAULT_DISPLAY, NULL);
  }
#endif
  if (host->display == EGL_NO_DISPLAY) {
    host->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  }
  if (host->display == EGL_NO_DISPLAY ||
      eglInitialize(host->display, NULL, NULL) != EGL_TRUE ||
      eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
    return false;
  }

  const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE,     8,               EGL_GREEN_SIZE,     8,
      EGL_BLUE_SIZE,    8,               EGL_ALPHA_SIZE,     8,
      EGL_DEPTH_SIZE,   24,              EGL_STENCIL_SIZE,   8,
      EGL_NONE,
  };
  EGLConfig config = NULL;
  EGLint config_count = 0;
  if (eglChooseConfig(host->display, config_attributes, &config, 1,
                      &config_count) != EGL_TRUE ||
      config_count == 0) {
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
  host->context = eglCreateContext(host->display, config, EGL_NO_CONTEXT,
                                   context_attributes);
  if (host->context == EGL_NO_CONTEXT) {
    return false;
  }
  const EGLint surface_attributes[] = {
      EGL_WIDTH, kWidth, EGL_HEIGHT, kHeight, EGL_NONE,
  };
  host->surface =
      eglCreatePbufferSurface(host->display, config, surface_attributes);
  return host->surface != EGL_NO_SURFACE;
}

static void destroy_opengl(opengl_host_s* host) {
  if (host->display == EGL_NO_DISPLAY) {
    return;
  }
  eglMakeCurrent(host->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (host->context != EGL_NO_CONTEXT) {
    eglDestroyContext(host->display, host->context);
  }
  if (host->surface != EGL_NO_SURFACE) {
    eglDestroySurface(host->display, host->surface);
  }
  eglTerminate(host->display);
}

int main(void) {
  if (sizeof(ghostty_surface_config_s) != 168 ||
      GHOSTTY_SURFACE_IO_MANUAL_MIRROR != 2) {
    fprintf(stderr, "unexpected Ghostty embedded ABI\n");
    return 2;
  }
  if (ghostty_init(0, NULL) != GHOSTTY_SUCCESS) {
    fprintf(stderr, "ghostty_init failed\n");
    return 3;
  }

  opengl_host_s host;
  if (!initialize_opengl(&host)) {
    fprintf(stderr, "EGL OpenGL 4.3 initialization failed (0x%x)\n",
            eglGetError());
    destroy_opengl(&host);
    return 4;
  }

  ghostty_config_t config = ghostty_config_new();
  if (!config) {
    destroy_opengl(&host);
    return 5;
  }
  ghostty_config_finalize(config);
  ghostty_runtime_config_s runtime = {0};
  runtime.userdata = &host;
  runtime.wakeup_cb = wakeup_cb;
  runtime.action_cb = action_cb;
  runtime.read_clipboard_cb = read_clipboard_cb;
  runtime.confirm_read_clipboard_cb = confirm_read_clipboard_cb;
  runtime.write_clipboard_cb = write_clipboard_cb;
  runtime.close_surface_cb = close_surface_cb;
  g_app = ghostty_app_new(&runtime, config);
  ghostty_config_free(config);
  if (!g_app) {
    destroy_opengl(&host);
    return 6;
  }

  ghostty_surface_config_s surface_config = ghostty_surface_config_new();
  surface_config.platform_tag = GHOSTTY_PLATFORM_OPENGL;
  surface_config.platform.opengl.userdata = &host;
  surface_config.platform.opengl.make_current = make_current_cb;
  surface_config.platform.opengl.clear_current = clear_current_cb;
  surface_config.platform.opengl.get_proc_address = get_proc_address_cb;
  surface_config.platform.opengl.swap_buffers = swap_buffers_cb;
  surface_config.userdata = &host;
  surface_config.scale_factor = 1.0;
  surface_config.context = GHOSTTY_SURFACE_CONTEXT_WINDOW;
  surface_config.io_mode = GHOSTTY_SURFACE_IO_MANUAL_MIRROR;
  surface_config.io_write_cb = io_write_cb;
  surface_config.io_write_userdata = &host;
  ghostty_surface_t surface = ghostty_surface_new(g_app, &surface_config);
  if (!surface) {
    fprintf(stderr, "ghostty_surface_new failed\n");
    ghostty_app_free(g_app);
    destroy_opengl(&host);
    return 7;
  }

  ghostty_app_set_focus(g_app, true);
  ghostty_surface_set_content_scale(surface, 1.0, 1.0);
  ghostty_surface_set_size(surface, kWidth, kHeight);
  ghostty_surface_set_focus(surface, true);
  const char output[] =
      "\x1b[2J\x1b[H\x1b[1;32mCMUX TUI OPENGL MIRROR OK\x1b[0m\r\n";
  ghostty_surface_process_output(surface, output, sizeof(output) - 1);
  ghostty_surface_draw(surface);

  const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
  for (int attempt = 0; attempt < 500; ++attempt) {
    ghostty_app_tick(g_app);
    if (atomic_load_explicit(&host.frames, memory_order_acquire) >= 2) {
      break;
    }
    nanosleep(&delay, NULL);
  }

  const unsigned frames =
      atomic_load_explicit(&host.frames, memory_order_acquire);
  const uint64_t checksum =
      atomic_load_explicit(&host.checksum, memory_order_acquire);
  ghostty_surface_free(surface);
  ghostty_app_free(g_app);
  destroy_opengl(&host);
  if (frames == 0 || checksum == 0) {
    fprintf(stderr, "no nonempty OpenGL frame (frames=%u checksum=%llu)\n",
            frames, (unsigned long long)checksum);
    return 8;
  }
  printf("ghostty OpenGL mirror OK: frames=%u checksum=%llu\n", frames,
         (unsigned long long)checksum);
  return 0;
}

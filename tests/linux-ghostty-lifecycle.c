// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later
// Functional Linux embedder regression: replay must replace terminal contents,
// and freeing interleaved surfaces must not invalidate a surviving renderer.
// Build against the pinned Ghostty fork's include/ and zig-out/lib/ directories.
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ghostty.h"

struct host {
  EGLDisplay display;
  EGLSurface drawable;
  EGLContext context;
  ghostty_surface_t terminal;
};

static EGLDisplay display;
static EGLConfig egl_config;
static ghostty_app_t app;

static bool make_current(void* userdata) {
  struct host* host = userdata;
  return eglMakeCurrent(host->display, host->drawable, host->drawable,
                        host->context) == EGL_TRUE;
}

static void clear_current(void* userdata) {
  struct host* host = userdata;
  assert(eglMakeCurrent(host->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT) == EGL_TRUE);
}

static void* get_proc(void* userdata, const char* name) {
  (void)userdata;
  return (void*)eglGetProcAddress(name);
}

static void swap_buffers(void* userdata) {
  struct host* host = userdata;
  assert(eglSwapBuffers(host->display, host->drawable) == EGL_TRUE);
}

static void wakeup(void* userdata) { (void)userdata; }

static bool action(ghostty_app_t a, ghostty_target_s target,
                   ghostty_action_s event) {
  (void)a;
  if (event.tag == GHOSTTY_ACTION_RENDER &&
      target.tag == GHOSTTY_TARGET_SURFACE) {
    ghostty_surface_draw(target.target.surface);
    return true;
  }
  return false;
}

static bool read_clipboard(void* userdata, ghostty_clipboard_e clipboard,
                           void* state) {
  (void)userdata;
  (void)clipboard;
  (void)state;
  return false;
}

static void confirm_clipboard(void* userdata, const char* text, void* state,
                              ghostty_clipboard_request_e request) {
  (void)userdata;
  (void)text;
  (void)state;
  (void)request;
}

static void write_clipboard(void* userdata, ghostty_clipboard_e clipboard,
                            const ghostty_clipboard_content_s* content,
                            size_t count, bool confirm) {
  (void)userdata;
  (void)clipboard;
  (void)content;
  (void)count;
  (void)confirm;
}

static void close_surface(void* userdata, bool alive) {
  (void)userdata;
  (void)alive;
}

static void io_write(void* userdata, const char* text, uintptr_t length) {
  (void)userdata;
  (void)text;
  (void)length;
}

static void tick(void) {
  const struct timespec delay = {.tv_nsec = 1000000};
  for (int i = 0; i < 10; ++i) {
    ghostty_app_tick(app);
    nanosleep(&delay, NULL);
  }
}

static void create_host(struct host* host) {
  const EGLint pbuffer[] = {EGL_WIDTH, 640, EGL_HEIGHT, 360, EGL_NONE};
  const EGLint context[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
                            EGL_CONTEXT_MINOR_VERSION_KHR, 3,
                            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
  *host = (struct host){.display = display};
  host->drawable = eglCreatePbufferSurface(display, egl_config, pbuffer);
  host->context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, context);
  assert(host->drawable != EGL_NO_SURFACE && host->context != EGL_NO_CONTEXT);
  ghostty_surface_config_s config = ghostty_surface_config_new();
  config.platform_tag = GHOSTTY_PLATFORM_OPENGL;
  config.platform.opengl = (ghostty_platform_opengl_s){
      host, make_current, clear_current, get_proc, swap_buffers};
  config.userdata = host;
  config.io_mode = GHOSTTY_SURFACE_IO_MANUAL_MIRROR;
  config.io_write_cb = io_write;
  host->terminal = ghostty_surface_new(app, &config);
  assert(host->terminal);
  ghostty_surface_set_size(host->terminal, 640, 360);
  tick();
}

static void free_host(struct host* host) {
  ghostty_surface_free(host->terminal);
  host->terminal = NULL;
  assert(eglDestroyContext(display, host->context));
  assert(eglDestroySurface(display, host->drawable));
  tick();
}

static void check_replay(struct host* host) {
  const char old[] = "STALE-CONTENT\r\n";
  const char replay[] = "\x1b" "c" "\x1b[3JFRESH-CONTENT\r\n";
  ghostty_surface_process_output(host->terminal, old, sizeof(old) - 1);
  ghostty_surface_process_output(host->terminal, replay, sizeof(replay) - 1);
  tick();
  const ghostty_selection_s selection = {
      .top_left = {GHOSTTY_POINT_SCREEN, GHOSTTY_POINT_COORD_TOP_LEFT, 0, 0},
      .bottom_right = {GHOSTTY_POINT_SCREEN, GHOSTTY_POINT_COORD_BOTTOM_RIGHT,
                      0, 0}};
  ghostty_text_s output = {0};
  assert(ghostty_surface_read_text(host->terminal, selection, &output));
  char* text = strndup(output.text, output.text_len);
  assert(text && strstr(text, "FRESH-CONTENT") && !strstr(text, "STALE-CONTENT"));
  free(text);
  ghostty_surface_free_text(host->terminal, &output);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                   EGL_DEFAULT_DISPLAY, NULL);
  assert(display != EGL_NO_DISPLAY && eglInitialize(display, NULL, NULL));
  assert(eglBindAPI(EGL_OPENGL_API));
  const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                          EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                          EGL_NONE};
  EGLint configs = 0;
  assert(eglChooseConfig(display, attrs, &egl_config, 1, &configs) && configs == 1);
  assert(ghostty_init(0, NULL) == GHOSTTY_SUCCESS);
  ghostty_config_t config = ghostty_config_new();
  const char options[] = "font-family = DejaVu Sans Mono\nfont-size = 12\n";
  ghostty_config_load_string(config, options, sizeof(options) - 1,
                             "cmux://linux-lifecycle-test");
  ghostty_config_finalize(config);
  assert(ghostty_config_diagnostics_count(config) == 0);
  const ghostty_runtime_config_s runtime = {
      .wakeup_cb = wakeup, .action_cb = action,
      .read_clipboard_cb = read_clipboard,
      .confirm_read_clipboard_cb = confirm_clipboard,
      .write_clipboard_cb = write_clipboard, .close_surface_cb = close_surface};
  app = ghostty_app_new(&runtime, config);
  assert(app);
  ghostty_config_free(config);
  for (int cycle = 0; cycle < 30; ++cycle) {
    struct host hosts[3];
    for (int i = 0; i < 3; ++i) {
      create_host(&hosts[i]);
      check_replay(&hosts[i]);
    }
    // Free a younger surface before an older one, then create another while
    // the oldest remains alive: this exercises the reported failing order.
    free_host(&hosts[1]);
    check_replay(&hosts[0]);
    create_host(&hosts[1]);
    free_host(&hosts[2]);
    free_host(&hosts[0]);
    free_host(&hosts[1]);
    printf("PASS lifecycle cycle %d\n", cycle + 1);
  }
  ghostty_app_free(app);
  assert(eglTerminate(display));
  puts("PASS replay replacement and 120 surface create/free operations");
  return 0;
}

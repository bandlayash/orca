// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLInterface/EmscriptenGL.h"

#include <cstdio>

#include "Common/Logging/Log.h"

GLContextEmscripten::~GLContextEmscripten()
{
  if (m_context)
  {
    emscripten_webgl_destroy_context(m_context);
    m_context = 0;
  }
}

bool GLContextEmscripten::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  std::fprintf(stderr, "[orca-gl] EmscriptenGL::Initialize: creating WebGL2 context on #canvas\n");
  EmscriptenWebGLContextAttributes attrs;
  emscripten_webgl_init_context_attributes(&attrs);
  attrs.majorVersion = 2;  // WebGL 2 = GLES 3.0
  attrs.minorVersion = 0;
  attrs.antialias = false;
  attrs.depth = true;
  attrs.stencil = true;
  attrs.alpha = false;
  attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
  attrs.enableExtensionsByDefault = true;
  // Allow WebGL context creation from worker threads (GPU thread in dual-core mode).
  attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
  attrs.renderViaOffscreenBackBuffer = true;

  m_context = emscripten_webgl_create_context("#canvas", &attrs);
  std::fprintf(stderr, "[orca-gl] EmscriptenGL: context=%lu\n", m_context);
  if (m_context <= 0)
  {
    std::fprintf(stderr, "[orca-gl] EmscriptenGL: FAILED context=%lu\n", m_context);
    ERROR_LOG_FMT(VIDEO, "EmscriptenGL: Failed to create WebGL 2 context (error {})", m_context);
    m_context = 0;
    return false;
  }

  EMSCRIPTEN_RESULT res = emscripten_webgl_make_context_current(m_context);
  if (res != EMSCRIPTEN_RESULT_SUCCESS)
  {
    ERROR_LOG_FMT(VIDEO, "EmscriptenGL: Failed to make context current (error {})", res);
    emscripten_webgl_destroy_context(m_context);
    m_context = 0;
    return false;
  }

  m_opengl_mode = Mode::OpenGLES;

  // Query the actual drawing-buffer size from the canvas rather than hardcoding 640x480.
  // This ensures the Presenter and system framebuffer start at the correct resolution.
  int w = 0, h = 0;
  emscripten_webgl_get_drawing_buffer_size(m_context, &w, &h);
  m_backbuffer_width = (w > 0) ? static_cast<u32>(w) : 640;
  m_backbuffer_height = (h > 0) ? static_cast<u32>(h) : 480;
  std::fprintf(stderr, "[orca-gl] EmscriptenGL: context current, mode=GLES, %ux%u\n",
               m_backbuffer_width, m_backbuffer_height);

  INFO_LOG_FMT(VIDEO, "EmscriptenGL: WebGL 2 context created ({}x{})", m_backbuffer_width,
               m_backbuffer_height);
  return true;
}

bool GLContextEmscripten::MakeCurrent()
{
  std::fprintf(stderr, "[orca-gl] EmscriptenGL::MakeCurrent\n");
  return emscripten_webgl_make_context_current(m_context) == EMSCRIPTEN_RESULT_SUCCESS;
}

bool GLContextEmscripten::ClearCurrent()
{
  return emscripten_webgl_make_context_current(0) == EMSCRIPTEN_RESULT_SUCCESS;
}

void GLContextEmscripten::Update()
{
  // Query the actual canvas drawing-buffer size so the renderer tracks resizes.
  int w = 0, h = 0;
  emscripten_webgl_get_drawing_buffer_size(m_context, &w, &h);
  if (w > 0 && h > 0)
  {
    m_backbuffer_width = static_cast<u32>(w);
    m_backbuffer_height = static_cast<u32>(h);
  }
}

void GLContextEmscripten::Swap()
{
  // No-op: Emscripten auto-presents the backbuffer at the end of each requestAnimationFrame tick.
}

void GLContextEmscripten::SwapInterval(int interval)
{
  // No-op: swap interval is controlled by the browser's vsync / rAF.
}

void* GLContextEmscripten::GetFuncAddress(const std::string& name)
{
  return reinterpret_cast<void*>(emscripten_webgl_get_proc_address(name.c_str()));
}

std::unique_ptr<GLContext> GLContextEmscripten::CreateSharedContext()
{
  // WebGL does not support shared contexts.
  return nullptr;
}

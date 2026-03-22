// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <emscripten/html5.h>
#include <string>

#include "Common/GL/GLContext.h"

class GLContextEmscripten final : public GLContext
{
public:
  ~GLContextEmscripten() override;

  bool IsHeadless() const override { return false; }

  std::unique_ptr<GLContext> CreateSharedContext() override;

  bool MakeCurrent() override;
  bool ClearCurrent() override;

  void Update() override;

  void Swap() override;
  void SwapInterval(int interval) override;

  void* GetFuncAddress(const std::string& name) override;

protected:
  bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core) override;

private:
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE m_context = 0;
};

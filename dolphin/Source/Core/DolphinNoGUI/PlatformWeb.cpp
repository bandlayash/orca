// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>

#include <emscripten/emscripten.h>

#include "Common/MsgHandler.h"
#include "Common/Config/Config.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "VideoCommon/VideoConfig.h"
#include "Core/Core.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"

namespace
{
class PlatformWeb final : public Platform
{
public:
  void SetTitle(const std::string& title) override;
  void MainLoop() override;
  void Tick();
  WindowSystemInfo GetWindowSystemInfo() const override;
};

void PlatformWeb::SetTitle(const std::string& title)
{
  std::fprintf(stdout, "%s\n", title.c_str());
}

void PlatformWeb::Tick()
{
  UpdateRunningFlag();
  Core::HostDispatchJobs(Core::System::GetInstance());
}

static void WebTick(void* arg)
{
  static_cast<PlatformWeb*>(arg)->Tick();
}

void PlatformWeb::MainLoop()
{
  emscripten_set_main_loop_arg(WebTick, this, 0, 1);
}

WindowSystemInfo PlatformWeb::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Headless;
  wsi.display_connection = nullptr;
  wsi.render_window = nullptr;
  wsi.render_surface = nullptr;
  return wsi;
}

}  // namespace

static Platform* s_web_platform = nullptr;

// On Emscripten, DefaultMsgHandler returns false for yes/no questions, which causes
// ASSERT_MSG → PanicYesNo → Crash() → __builtin_trap() → WASM unreachable.
// Register a handler that always continues (returns true) so assertions don't abort.
static bool WebMsgAlertHandler(const char* caption, const char* text, bool /*yes_no*/,
                               Common::MsgType /*style*/)
{
  std::fprintf(stderr, "[dolphin] %s: %s\n", caption, text);
  return true;  // always "yes/continue" - never call Crash()
}

std::unique_ptr<Platform> Platform::CreateWebPlatform()
{
  auto platform = std::make_unique<PlatformWeb>();
  s_web_platform = platform.get();

  Common::RegisterMsgAlertHandler(WebMsgAlertHandler);

  // Force settings appropriate for the web environment.
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::CachedInterpreter);
  Config::SetBase(Config::MAIN_DSP_HLE, true);
  Config::SetBase(Config::MAIN_SKIP_IPL, true);

  // Reduce GPU workload: native resolution, no MSAA/SSAA, fast depth.
  Config::SetBase(Config::GFX_EFB_SCALE, 1);
  Config::SetBase(Config::GFX_MSAA, 1u);
  Config::SetBase(Config::GFX_SSAA, false);
  Config::SetBase(Config::GFX_FAST_DEPTH_CALC, true);
  Config::SetBase(Config::GFX_ENABLE_PIXEL_LIGHTING, false);

  // EFB/XFB hacks: keep copies on GPU, skip expensive RAM readbacks.
  Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, true);
  Config::SetBase(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, true);
  Config::SetBase(Config::GFX_HACK_DEFER_EFB_COPIES, true);
  Config::SetBase(Config::GFX_HACK_EFB_ACCESS_ENABLE, false);

  // Synchronous shader compilation (no background threads on web).
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::Synchronous);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, false);

  return platform;
}

extern "C" EMSCRIPTEN_KEEPALIVE int dolphin_load_rom(const char* path)
{
  auto boot = BootParameters::GenerateFromFile(
      std::string(path), BootSessionData(std::nullopt, DeleteSavestateAfterBoot::No));
  if (!boot)
    return -1;

  if (!s_web_platform)
    return -3;

  const WindowSystemInfo wsi = s_web_platform->GetWindowSystemInfo();
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
    return -2;

  return 0;
}

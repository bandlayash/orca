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
#include "VideoCommon/VideoBackendBase.h"
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
  std::fprintf(stderr, "[orca] GetWindowSystemInfo: type=Headless\n");
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

  return platform;
}

// Apply web-specific config overrides.  MUST be called AFTER UICommon::Init()
// so that Config::Init() has created the Base layer and the config system is
// ready to accept SetBase() calls.
void Platform::ApplyWebConfigOverrides()
{
  std::fprintf(stderr, "[orca] ApplyWebConfigOverrides: setting web defaults\n");

  // Force settings appropriate for the web environment.
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::AotWasm);
  Config::SetBase(Config::MAIN_DSP_HLE, true);
  Config::SetBase(Config::MAIN_SKIP_IPL, true);

  // Force OGL backend (maps to WebGL on Emscripten).
  Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("OGL"));

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

  // Force single-core mode (CPU and GPU on same thread).
  Config::SetBase(Config::MAIN_CPU_THREAD, false);
  Config::SetBase(Config::MAIN_SYNC_ON_SKIP_IDLE, true);

  // Re-activate the video backend now that we have overridden MAIN_GFX_BACKEND.
  VideoBackendBase::ActivateBackend(Config::Get(Config::MAIN_GFX_BACKEND));
  std::fprintf(stderr, "[orca] ApplyWebConfigOverrides: video backend set to %s\n",
               Config::Get(Config::MAIN_GFX_BACKEND).c_str());
}

extern "C" EMSCRIPTEN_KEEPALIVE int dolphin_load_rom(const char* path)
{
  std::fprintf(stderr, "[orca] dolphin_load_rom: path=%s\n", path);
  auto boot = BootParameters::GenerateFromFile(
      std::string(path), BootSessionData(std::nullopt, DeleteSavestateAfterBoot::No));
  if (!boot)
    return -1;
  std::fprintf(stderr, "[orca] dolphin_load_rom: boot params created\n");

  if (!s_web_platform)
    return -3;

  // Safety-critical overrides: use SetCurrent (CurrentRun layer) so that game INIs
  // loaded during BootCore cannot override them.  Game INIs sit at GlobalGame/LocalGame
  // layers which are lower priority than CurrentRun.  Without this, a game INI that
  // sets CPUThread=True would enable dual-core mode, spawning a separate GPU thread
  // whose proxied WebGL calls deadlock against the main thread under Emscripten.
  Config::SetCurrent(Config::MAIN_CPU_THREAD, false);
  Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("OGL"));
  Config::SetCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::AotWasm);
  Config::SetCurrent(Config::MAIN_DSP_HLE, true);

  const WindowSystemInfo wsi = s_web_platform->GetWindowSystemInfo();
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
    return -2;
  std::fprintf(stderr, "[orca] dolphin_load_rom: BootCore returned successfully\n");

  return 0;
}

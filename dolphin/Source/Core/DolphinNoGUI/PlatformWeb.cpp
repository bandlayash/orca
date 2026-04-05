// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>

#include <emscripten/emscripten.h>
#include <emscripten/em_js.h>
#include <emscripten/html5.h>

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

// --- Web Input: keyboard + gamepad ---
// Button constants matching GCPadStatus.h PadButton enum
// PAD_BUTTON_LEFT=0x0001, RIGHT=0x0002, DOWN=0x0004, UP=0x0008
// PAD_TRIGGER_Z=0x0010, PAD_TRIGGER_R=0x0020, PAD_TRIGGER_L=0x0040
// PAD_BUTTON_A=0x0100, B=0x0200, X=0x0400, Y=0x0800, START=0x1000

EM_JS(void, web_input_init, (), {
  // Keyboard → pad 0
  var keyMap = {
    'KeyX':      0x0100,  // A
    'KeyZ':      0x0200,  // B
    'KeyS':      0x0400,  // X
    'KeyA':      0x0800,  // Y
    'Enter':     0x1000,  // Start
    'KeyC':      0x0010,  // Z
    'KeyQ':      0x0040,  // L
    'KeyW':      0x0020,  // R
    'ArrowUp':   0x0008,  // D-pad Up
    'ArrowDown': 0x0004,  // D-pad Down
    'ArrowLeft': 0x0001,  // D-pad Left
    'ArrowRight':0x0002   // D-pad Right
  };

  // Track which arrow keys are pressed for analog stick emulation
  var stickKeys = { up: false, down: false, left: false, right: false };

  function updateStick() {
    var cx = 128, cy = 128;
    if (stickKeys.left)  cx -= 127;
    if (stickKeys.right) cx += 127;
    if (stickKeys.up)    cy += 127;
    if (stickKeys.down)  cy -= 127;
    // Clamp
    if (cx < 0) cx = 0;
    if (cx > 255) cx = 255;
    if (cy < 0) cy = 0;
    if (cy > 255) cy = 255;
    Module._web_set_stick(0, 0, cx, cy);
  }

  document.addEventListener('keydown', function(e) {
    if (e.repeat) return;
    var btn = keyMap[e.code];
    if (btn !== undefined) {
      Module._web_set_button(0, btn, 1);
      e.preventDefault();
    }
    // Arrow keys also drive the main stick
    if (e.code === 'ArrowUp')    { stickKeys.up = true; updateStick(); }
    if (e.code === 'ArrowDown')  { stickKeys.down = true; updateStick(); }
    if (e.code === 'ArrowLeft')  { stickKeys.left = true; updateStick(); }
    if (e.code === 'ArrowRight') { stickKeys.right = true; updateStick(); }
  });

  document.addEventListener('keyup', function(e) {
    var btn = keyMap[e.code];
    if (btn !== undefined) {
      Module._web_set_button(0, btn, 0);
      e.preventDefault();
    }
    if (e.code === 'ArrowUp')    { stickKeys.up = false; updateStick(); }
    if (e.code === 'ArrowDown')  { stickKeys.down = false; updateStick(); }
    if (e.code === 'ArrowLeft')  { stickKeys.left = false; updateStick(); }
    if (e.code === 'ArrowRight') { stickKeys.right = false; updateStick(); }
  });

  // Gamepad API polling (connected to pad 0)
  // Standard gamepad mapping: https://w3c.github.io/gamepad/#remapping
  var gpPollId = null;
  function pollGamepad() {
    var gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
    var gp = gamepads[0];
    if (!gp) { gpPollId = requestAnimationFrame(pollGamepad); return; }

    // Buttons: [A, B, X, Y, LB, RB, LT, RT, Back, Start, LS, RS, Up, Down, Left, Right]
    var gpButtons = [
      [0,  0x0100],  // A → PAD_BUTTON_A
      [1,  0x0200],  // B → PAD_BUTTON_B
      [2,  0x0400],  // X → PAD_BUTTON_X
      [3,  0x0800],  // Y → PAD_BUTTON_Y
      [4,  0x0040],  // LB → PAD_TRIGGER_L
      [5,  0x0020],  // RB → PAD_TRIGGER_R
      [7,  0x0010],  // RT → PAD_TRIGGER_Z (R shoulder as Z)
      [9,  0x1000],  // Start → PAD_BUTTON_START
      [12, 0x0008],  // D-Up → PAD_BUTTON_UP
      [13, 0x0004],  // D-Down → PAD_BUTTON_DOWN
      [14, 0x0001],  // D-Left → PAD_BUTTON_LEFT
      [15, 0x0002],  // D-Right → PAD_BUTTON_RIGHT
    ];

    for (var i = 0; i < gpButtons.length; i++) {
      var idx = gpButtons[i][0];
      var mask = gpButtons[i][1];
      if (idx < gp.buttons.length) {
        Module._web_set_button(0, mask, gp.buttons[idx].pressed ? 1 : 0);
      }
    }

    // Left stick → main stick (axes 0,1)
    if (gp.axes.length >= 2) {
      var lx = Math.round(gp.axes[0] * 127 + 128);
      // Y-axis inverted: browser down=+1 but GC up=+Y
      var ly = Math.round(-gp.axes[1] * 127 + 128);
      if (lx < 0) lx = 0; if (lx > 255) lx = 255;
      if (ly < 0) ly = 0; if (ly > 255) ly = 255;
      Module._web_set_stick(0, 0, lx, ly);
    }

    // Right stick → C-stick (axes 2,3)
    if (gp.axes.length >= 4) {
      var rx = Math.round(gp.axes[2] * 127 + 128);
      var ry = Math.round(-gp.axes[3] * 127 + 128);
      if (rx < 0) rx = 0; if (rx > 255) rx = 255;
      if (ry < 0) ry = 0; if (ry > 255) ry = 255;
      Module._web_set_stick(0, 1, rx, ry);
    }

    // Analog triggers (axes 4,5 or buttons 6,7)
    if (gp.buttons.length > 6) {
      Module._web_set_trigger(0, 0, Math.round(gp.buttons[6].value * 255));  // LT
      Module._web_set_trigger(0, 1, Math.round(gp.buttons[7].value * 255));  // RT
    }

    gpPollId = requestAnimationFrame(pollGamepad);
  }

  window.addEventListener('gamepadconnected', function(e) {
    console.log('[orca-input] Gamepad connected:', e.gamepad.id);
    if (!gpPollId) pollGamepad();
  });

  window.addEventListener('gamepaddisconnected', function(e) {
    console.log('[orca-input] Gamepad disconnected:', e.gamepad.id);
  });

  // Start polling immediately in case gamepad was already connected
  pollGamepad();

  console.log('[orca-input] Web input initialized (keyboard + gamepad)');
});

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

  // Initialize keyboard and gamepad input handlers
  web_input_init();

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

  // Enable dual-core mode (separate CPU and GPU threads).  The GPU thread creates its
  // WebGL context directly on an OffscreenCanvas (PROXY_DISALLOW), so there is no
  // deadlock between the GPU worker and the main thread.  This requires the linker
  // flags -sOFFSCREENCANVAS_SUPPORT=1 and -sOFFSCREENCANVASES=['#canvas'].
  Config::SetBase(Config::MAIN_CPU_THREAD, true);
  Config::SetBase(Config::MAIN_DSP_THREAD, true);
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
  // layers which are lower priority than CurrentRun.  Dual-core is now safe because
  // the GPU thread creates its WebGL context directly on an OffscreenCanvas
  // (PROXY_DISALLOW) instead of proxying to the main thread.
  Config::SetCurrent(Config::MAIN_CPU_THREAD, true);
  Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("OGL"));
  Config::SetCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::AotWasm);
  Config::SetCurrent(Config::MAIN_DSP_HLE, true);
  Config::SetCurrent(Config::MAIN_DSP_THREAD, true);

  const WindowSystemInfo wsi = s_web_platform->GetWindowSystemInfo();
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
    return -2;
  std::fprintf(stderr, "[orca] dolphin_load_rom: BootCore returned successfully\n");

  return 0;
}

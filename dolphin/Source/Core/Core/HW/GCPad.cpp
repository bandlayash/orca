// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCPad.h"

#include "Common/Common.h"
#include "Core/HW/GCPadEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <cstring>

// Web input state: written by JS keyboard/gamepad handlers, read by GetStatus()
static GCPadStatus s_web_pad[4];

extern "C" EMSCRIPTEN_KEEPALIVE void web_set_button(int pad, u16 button, int pressed)
{
  if (pad < 0 || pad >= 4)
    return;
  if (pressed)
    s_web_pad[pad].button |= button;
  else
    s_web_pad[pad].button &= ~button;
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_set_stick(int pad, int stick, u8 x, u8 y)
{
  if (pad < 0 || pad >= 4)
    return;
  if (stick == 0)
  {
    s_web_pad[pad].stickX = x;
    s_web_pad[pad].stickY = y;
  }
  else
  {
    s_web_pad[pad].substickX = x;
    s_web_pad[pad].substickY = y;
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_set_trigger(int pad, int trigger, u8 value)
{
  if (pad < 0 || pad >= 4)
    return;
  if (trigger == 0)
    s_web_pad[pad].triggerLeft = value;
  else
    s_web_pad[pad].triggerRight = value;
}
#endif  // __EMSCRIPTEN__

namespace Pad
{
static InputConfig s_config("GCPadNew", _trans("Pad"), "GCPad", "Pad");
InputConfig* GetConfig()
{
  return &s_config;
}

void Shutdown()
{
  s_config.UnregisterHotplugCallback();

  s_config.ClearControllers();
}

void Initialize()
{
  if (s_config.ControllersNeedToBeCreated())
  {
    for (unsigned int i = 0; i < 4; ++i)
      s_config.CreateController<GCPad>(i);
  }

  s_config.RegisterHotplugCallback();

  // Load the saved controller config
  s_config.LoadConfig();
}

void LoadConfig()
{
  s_config.LoadConfig();
}

void GenerateDynamicInputTextures()
{
  s_config.GenerateControllerTextures();
}

bool IsInitialized()
{
  return !s_config.ControllersNeedToBeCreated();
}

GCPadStatus GetStatus(int pad_num)
{
#ifdef __EMSCRIPTEN__
  if (pad_num >= 0 && pad_num < 4)
  {
    GCPadStatus status = s_web_pad[pad_num];
    status.isConnected = true;
    // Set analog values for digital button presses
    if (status.button & PAD_BUTTON_A)
      status.analogA = 0xFF;
    if (status.button & PAD_BUTTON_B)
      status.analogB = 0xFF;
    if (status.button & PAD_TRIGGER_L)
      status.triggerLeft = std::max(status.triggerLeft, u8(0xFF));
    if (status.button & PAD_TRIGGER_R)
      status.triggerRight = std::max(status.triggerRight, u8(0xFF));
    return status;
  }
#endif
  return static_cast<GCPad*>(s_config.GetController(pad_num))->GetInput();
}

ControllerEmu::ControlGroup* GetGroup(int pad_num, PadGroup group)
{
  return static_cast<GCPad*>(s_config.GetController(pad_num))->GetGroup(group);
}

void Rumble(const int pad_num, const ControlState strength)
{
  static_cast<GCPad*>(s_config.GetController(pad_num))->SetOutput(strength);
}

void ResetRumble(const int pad_num)
{
  static_cast<GCPad*>(s_config.GetController(pad_num))->SetOutput(0.0);
}

bool GetMicButton(const int pad_num)
{
  return static_cast<GCPad*>(s_config.GetController(pad_num))->GetMicButton();
}
}  // namespace Pad

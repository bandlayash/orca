// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stub implementations of Common::ENet wrappers for Emscripten.
// NetPlay is not supported in the browser build; these stubs satisfy the linker.

#ifdef __EMSCRIPTEN__

#include "Common/ENet.h"

namespace Common::ENet
{
void WakeupThread(ENetHost*)
{
}

int ENET_CALLBACK InterceptCallback(ENetHost*, ENetEvent*)
{
  return 0;
}

bool SendPacket(ENetPeer*, const sf::Packet&, u8)
{
  return false;
}
}  // namespace Common::ENet

#endif  // __EMSCRIPTEN__

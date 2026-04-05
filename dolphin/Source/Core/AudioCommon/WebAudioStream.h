// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __EMSCRIPTEN__

#include "AudioCommon/SoundStream.h"
#include "Common/CommonTypes.h"

class WebAudioStream final : public SoundStream
{
public:
  ~WebAudioStream() override;

  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

  static bool IsValid() { return true; }

  // Called from JS audio callback to fill the sample buffer
  s16* GetSampleBuffer(int num_samples);

private:
  s16* m_buffer = nullptr;
  int m_buffer_size = 0;
};

#endif  // __EMSCRIPTEN__

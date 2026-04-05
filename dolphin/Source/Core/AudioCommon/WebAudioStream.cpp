// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef __EMSCRIPTEN__

#include "AudioCommon/WebAudioStream.h"

#include <cstdlib>
#include <emscripten.h>
#include <emscripten/em_js.h>

#include "Common/Logging/Log.h"

// Global pointer for JS callback access
static WebAudioStream* s_instance = nullptr;

// C function exported to JS: fills a sample buffer from the Mixer.
// Returns pointer to interleaved s16 stereo PCM data.
extern "C" EMSCRIPTEN_KEEPALIVE short* webaudio_get_samples(int num_samples)
{
  if (!s_instance)
    return nullptr;
  return s_instance->GetSampleBuffer(num_samples);
}

// Initialize the Web Audio API: AudioContext + ScriptProcessorNode
EM_JS(void, webaudio_init, (), {
  if (Module._webAudioCtx)
    return;

  try {
    var ctx = new (window.AudioContext || window.webkitAudioContext)({sampleRate : 48000});
    var gainNode = ctx.createGain();
    gainNode.connect(ctx.destination);

    // ScriptProcessorNode: 4096 samples buffer, 0 input channels, 2 output channels
    var processor = ctx.createScriptProcessor(4096, 0, 2);
    processor.onaudioprocess = function(e) {
      var left = e.outputBuffer.getChannelData(0);
      var right = e.outputBuffer.getChannelData(1);
      var numSamples = left.length;

      var bufPtr = Module._webaudio_get_samples(numSamples);
      if (bufPtr === 0) {
        // No data — fill with silence
        left.fill(0);
        right.fill(0);
        return;
      }

      // bufPtr points to s16 interleaved stereo data in WASM memory
      var buf = Module.HEAP16;
      var offset = bufPtr >> 1;  // byte offset → i16 offset
      for (var i = 0; i < numSamples; i++) {
        left[i] = buf[offset + i * 2] / 32768.0;
        right[i] = buf[offset + i * 2 + 1] / 32768.0;
      }
    };
    processor.connect(gainNode);

    Module._webAudioCtx = ctx;
    Module._webAudioGain = gainNode;
    Module._webAudioProcessor = processor;
    console.log("[orca-audio] WebAudio initialized: sampleRate=" + ctx.sampleRate);
  } catch (e) {
    console.error("[orca-audio] WebAudio init failed:", e);
  }
});

EM_JS(void, webaudio_resume, (), {
  if (Module._webAudioCtx && Module._webAudioCtx.state === 'suspended') {
    Module._webAudioCtx.resume();
  }
});

EM_JS(void, webaudio_suspend, (), {
  if (Module._webAudioCtx && Module._webAudioCtx.state === 'running') {
    Module._webAudioCtx.suspend();
  }
});

EM_JS(void, webaudio_set_volume, (float vol), {
  if (Module._webAudioGain) {
    Module._webAudioGain.gain.value = vol;
  }
});

EM_JS(void, webaudio_destroy, (), {
  if (Module._webAudioProcessor) {
    Module._webAudioProcessor.disconnect();
    Module._webAudioProcessor = null;
  }
  if (Module._webAudioGain) {
    Module._webAudioGain.disconnect();
    Module._webAudioGain = null;
  }
  if (Module._webAudioCtx) {
    Module._webAudioCtx.close();
    Module._webAudioCtx = null;
  }
});

WebAudioStream::~WebAudioStream()
{
  webaudio_destroy();
  std::free(m_buffer);
  m_buffer = nullptr;
  s_instance = nullptr;
}

bool WebAudioStream::Init()
{
  s_instance = this;
  webaudio_init();
  INFO_LOG_FMT(AUDIO, "WebAudioStream initialized");
  return true;
}

bool WebAudioStream::SetRunning(bool running)
{
  if (running)
    webaudio_resume();
  else
    webaudio_suspend();
  return true;
}

void WebAudioStream::SetVolume(int volume)
{
  // Volume: 0-100 → 0.0-1.0
  float vol = static_cast<float>(volume) / 100.0f;
  webaudio_set_volume(vol);
}

s16* WebAudioStream::GetSampleBuffer(int num_samples)
{
  const int needed = num_samples * 2;  // stereo
  if (m_buffer_size < needed)
  {
    std::free(m_buffer);
    m_buffer_size = needed;
    m_buffer = static_cast<s16*>(std::malloc(m_buffer_size * sizeof(s16)));
  }
  if (!m_mixer || !m_buffer)
    return nullptr;
  m_mixer->Mix(m_buffer, num_samples);
  return m_buffer;
}

#endif  // __EMSCRIPTEN__

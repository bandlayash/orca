var Module = {
  // The canvas element is registered here so that Emscripten can transfer it as
  // an OffscreenCanvas to the GPU worker thread (dual-core mode).  The linker
  // flags -sOFFSCREENCANVAS_SUPPORT=1 and -sOFFSCREENCANVASES_TO_PTHREAD='#canvas' tell
  // the runtime to perform the transfer when the GPU thread creates a WebGL
  // context on "#canvas".
  canvas: document.getElementById('canvas'),
  onDolphinReady: function() {
    document.getElementById('rom-input').disabled = false;
    document.getElementById('status').textContent = 'Ready \u2014 select a WBFS file';
  },
  print: function(t) { console.log('[dolphin]', t); },
  printErr: function(t) { console.warn('[dolphin]', t); },
};

document.getElementById('rom-input').addEventListener('change', async function(e) {
  var file = e.target.files[0];
  if (!file) return;

  document.getElementById('rom-input').disabled = true;
  document.getElementById('status').textContent = 'Loading ' + file.name + '\u2026';

  var data = new Uint8Array(await file.arrayBuffer());
  try { Module.FS.mkdir('/games'); } catch (_) {}
  Module.FS.writeFile('/games/game.wbfs', data);

  document.getElementById('status').textContent = 'Starting emulation\u2026';
  var rc = Module.ccall('dolphin_load_rom', 'number', ['string'], ['/games/game.wbfs']);
  if (rc !== 0) {
    document.getElementById('status').textContent = 'Boot failed (code ' + rc + ')';
    document.getElementById('rom-input').disabled = false;
  } else {
    document.getElementById('status').textContent = 'Running';
  }
});

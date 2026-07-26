// Auto-served at GET / by gui_main.cpp. This file is a single C++ raw string literal
// (R"HTML( ... )HTML") so it concatenates onto `static const char* kGuiHtml =`.
R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hermes — Audio Test Console</title>
<style>
  :root { --bg:#0f1216; --panel:#171c23; --ink:#e6edf3; --mut:#8b97a7; --acc:#4ea1ff; --ok:#3fb950; --warn:#d29922; }
  * { box-sizing:border-box; }
  body { margin:0; font:14px/1.45 ui-sans-serif,system-ui,Segoe UI,Roboto,Helvetica,Arial; background:var(--bg); color:var(--ink); }
  header { padding:16px 22px; border-bottom:1px solid #232b35; display:flex; align-items:baseline; gap:12px; }
  header h1 { font-size:17px; margin:0; }
  header .sub { color:var(--mut); font-size:12px; }
  main { max-width:880px; margin:0 auto; padding:22px; display:grid; gap:18px; grid-template-columns:1fr 1fr; }
  .card { background:var(--panel); border:1px solid #232b35; border-radius:10px; padding:16px; }
  .card.full { grid-column:1 / -1; }
  .card h2 { font-size:12px; text-transform:uppercase; letter-spacing:.08em; color:var(--mut); margin:0 0 12px; }
  label { display:block; font-size:12px; color:var(--mut); margin:10px 0 4px; }
  select, button, input[type=range] { font:inherit; }
  select { width:100%; padding:8px; background:#0d1117; color:var(--ink); border:1px solid #30363d; border-radius:7px; }
  .row { display:flex; gap:8px; flex-wrap:wrap; }
  button { cursor:pointer; padding:9px 14px; border-radius:7px; border:1px solid #30363d; background:#21262d; color:var(--ink); }
  button:hover { border-color:var(--acc); }
  button.primary { background:var(--acc); border-color:var(--acc); color:#04101f; font-weight:600; }
  button.danger { border-color:#5a2a2a; }
  button.danger:hover { border-color:#f85149; color:#f85149; }
  .modes { display:grid; grid-template-columns:1fr 1fr; gap:8px; }
  .modes button.active { background:#1f6feb33; border-color:var(--acc); color:var(--acc); }
  .vol { display:flex; align-items:center; gap:12px; }
  .vol input { flex:1; }
  .vol .val { width:52px; text-align:right; color:var(--acc); font-variant-numeric:tabular-nums; }
  #log { grid-column:1 / -1; }
  #feed { height:190px; overflow:auto; background:#0d1117; border:1px solid #30363d; border-radius:7px; padding:10px; font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace; color:#c9d1d9; white-space:pre-wrap; }
  .pill { font-size:11px; padding:2px 8px; border-radius:999px; background:#21262d; color:var(--mut); }
  .hint { color:var(--mut); font-size:11px; margin-top:8px; }
</style>
</head>
<body>
<header>
  <h1>Hermes · Audio Test Console</h1>
  <span class="sub">web UI → control CMsg on the bus → <code>hermes.abox</code></span>
  <span class="pill" id="status" style="margin-left:auto">connecting…</span>
</header>
<main>
  <section class="card">
    <h2>Playback</h2>
    <label for="file">Sample file</label>
    <select id="file"></select>
    <div class="row" style="margin-top:12px">
      <button class="primary" onclick="play()">▶ Play</button>
      <button class="danger" onclick="send('stop')">■ Stop</button>
    </div>
    <p class="hint">Feeds the WAV into <code>hermes.abox</code> via <code>pw-play</code>, and sends <code>START_CAPTURE</code>.</p>
  </section>

  <section class="card">
    <h2>Engine mode</h2>
    <div class="modes" id="modes">
      <button data-m="0" onclick="mode(0)">Keyword Listening</button>
      <button data-m="1" onclick="mode(1)">Barge-In Muting</button>
      <button data-m="2" onclick="mode(2)">Cloud Streaming</button>
      <button data-m="3" onclick="mode(3)">System Reset</button>
    </div>
    <p class="hint"><code>SET_MODE</code> → AUDIO_CORE (atomic mode swap, approach B).</p>
  </section>

  <section class="card">
    <h2>Volume</h2>
    <div class="vol">
      <input id="vol" type="range" min="0" max="150" value="100" oninput="volLabel()" onchange="volume()">
      <span class="val" id="volval">100%</span>
    </div>
    <p class="hint"><code>SET_VOLUME</code> → master output gain (0–1.5×) in the RT egress.</p>
  </section>

  <section class="card">
    <h2>Session &amp; barge-in</h2>
    <div class="row">
      <button onclick="send('bargein')">⚡ Barge-In</button>
      <button onclick="send('session_start')">Start Session</button>
      <button onclick="send('session_cancel')">Cancel Session</button>
      <button class="danger" onclick="send('reset')">Reset Pipeline</button>
    </div>
    <p class="hint">PTT-style session control → SUPERVISOR; barge-in ducks playback (URGENT lane).</p>
  </section>

  <section class="card full">
    <h2>Voice STT — standalone direct path (no pipeline)</h2>
    <div class="row" style="align-items:center">
      <button id="recBtn" class="primary" onclick="toggleRecord()">● Record</button>
      <span id="recStatus" style="color:var(--mut);font-size:12px;padding:0 12px">idle — click Record, speak, click again to transcribe</span>
    </div>
    <div id="transcript" style="margin-top:10px;padding:10px;background:#0d1117;border:1px solid #30363d;border-radius:7px;min-height:36px;font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;color:#c9d1d9"></div>
    <p class="hint">Browser mic → WAV → target sherpa-onnx (streaming zipformer 20M int8). QEMU: ~4s model load + 4.5× RT inference per clip. Mic needs a secure context: open via localhost / SSH port-forward (plain http://<ip> blocks getUserMedia).</p>
  </section>

  <section class="card full">
    <h2>Speak to Aria — full pipeline test</h2>
    <div class="row" style="align-items:center;gap:16px">
      <button id="speakBtn" class="primary" style="min-width:160px" onclick="toggleSpeak()">🎤 Hold to Speak</button>
      <span id="speakStatus" style="color:var(--mut);font-size:12px">idle — click to record, click again to send through the DSP chain → STT → LLM → TTS</span>
    </div>
    <p class="hint">Browser mic → WAV → <code>pw-play --target hermes.abox</code> (beamform→AEC→capgate) → llm_connector (resident STT → Groq → Piper TTS). Uses <code>START_SESSION</code> to bypass wake word for testing.</p>
  </section>

  <section class="card full">
    <h2>Conversation <span class="pill" id="convStatus">idle</span></h2>
    <div style="display:grid;gap:10px">
      <div style="background:#0d1117;border:1px solid #30363d;border-radius:7px;padding:12px">
        <div style="font-size:11px;color:var(--mut);margin-bottom:6px;text-transform:uppercase;letter-spacing:.06em">You</div>
        <div id="convTranscript" style="font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;color:#c9d1d9;min-height:22px">—</div>
      </div>
      <div style="background:#0d1117;border:1px solid #1f6feb;border-radius:7px;padding:12px">
        <div style="font-size:11px;color:#4ea1ff;margin-bottom:6px;text-transform:uppercase;letter-spacing:.06em">Aria</div>
        <div id="convReply" style="font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;color:#c9d1d9;min-height:22px">—</div>
      </div>
    </div>
  </section>

  <section class="card full" id="log">
    <h2>Event feed <span class="pill" id="count">0</span></h2>
    <div id="feed"></div>
  </section>
</main>

<script>
const $ = s => document.querySelector(s);
async function api(path, opts) {
  const r = await fetch(path, opts);
  return r.json();
}
async function send(action, extra={}) {
  try {
    const j = await api('/api/cmd', {method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify(Object.assign({action}, extra))});
    if (!j.ok) flash(j.msg || j.err || 'failed', true);
  } catch(e) { flash('request failed: '+e, true); }
}
function play()  { const f = $('#file').value; if (f) send('play', {file:f}); }
function mode(m) {
  send('mode', {value:m});
  document.querySelectorAll('#modes button').forEach(b => b.classList.toggle('active', +b.dataset.m === m));
}
function volume() { send('volume', {value: +$('#vol').value}); }
function volLabel() { $('#volval').textContent = $('#vol').value + '%'; }

let lastLen = 0, feedEl;
function flash(msg, bad) {
  const el = $('#status'); el.textContent = msg; el.style.color = bad ? '#f85149' : '#3fb950';
  setTimeout(() => { el.textContent = 'ready'; el.style.color = ''; }, 2500);
}
async function loadSamples() {
  try {
    const list = await api('/api/samples');
    const sel = $('#file'); sel.innerHTML = '';
    list.forEach(n => { const o = document.createElement('option'); o.value = o.textContent = n; sel.appendChild(o); });
    $('#status').textContent = list.length ? 'ready' : 'no samples found';
    $('#status').style.color = list.length ? '#3fb950' : '#d29922';
  } catch(e) { $('#status').textContent = 'offline'; $('#status').style.color = '#f85149'; }
}
async function pollEvents() {
  try {
    const ev = await api('/api/events');
    if (ev.length !== lastLen) {
      lastLen = ev.length;
      $('#feed').textContent = ev.join('\n');
      $('#count').textContent = ev.length;
      $('#feed').scrollTop = $('#feed').scrollHeight;
    }
  } catch(e) {}
}
loadSamples();
mode(0);
volLabel();
setInterval(pollEvents, 700);
pollEvents();

// ── Speak-to-Aria (full pipeline) ─────────────────────────────────────────────
let _spkActive=false, _spkCtx, _spkProc, _spkStream, _spkChunks=[];

async function toggleSpeak() {
  if (!_spkActive) {
    try {
      _spkStream = await navigator.mediaDevices.getUserMedia({audio:{channelCount:1,sampleRate:48000},video:false});
      _spkCtx = new (window.AudioContext||window.webkitAudioContext)({sampleRate:48000});
      const src = _spkCtx.createMediaStreamSource(_spkStream);
      _spkProc = _spkCtx.createScriptProcessor(4096, 1, 1);
      _spkChunks = [];
      _spkProc.onaudioprocess = e => _spkChunks.push(new Float32Array(e.inputBuffer.getChannelData(0)));
      src.connect(_spkProc); _spkProc.connect(_spkCtx.destination);
      _spkActive = true;
      $('#speakBtn').textContent = '■ Stop & Send';
      $('#speakBtn').className = 'danger';
      $('#speakStatus').textContent = 'recording… speak now, then click Stop & Send';
      $('#speakStatus').style.color = '#3fb950';
    } catch(e) { flash('mic error: '+e, true); }
  } else {
    _spkProc.disconnect(); _spkStream.getTracks().forEach(t=>t.stop());
    _spkActive = false;
    const sr = _spkCtx.sampleRate;
    await _spkCtx.close();
    const wav = _buildWav(_spkChunks, sr);
    const secs = ((wav.byteLength-44)/(sr*2)).toFixed(1);
    $('#speakBtn').textContent = '🎤 Hold to Speak'; $('#speakBtn').className = 'primary';
    $('#speakStatus').textContent = `sending ${secs}s → DSP pipeline…`;
    $('#speakStatus').style.color = 'var(--acc)';
    try {
      const r = await fetch('/api/speak', {method:'POST', body:wav, headers:{'Content-Type':'audio/wav'}});
      const j = await r.json();
      $('#speakStatus').textContent = j.ok ? 'audio sent → listening for STT…' : 'error: '+(j.err||'unknown');
      $('#speakStatus').style.color = j.ok ? 'var(--mut)' : '#f85149';
    } catch(e) {
      $('#speakStatus').textContent = 'error: '+e; $('#speakStatus').style.color = '#f85149';
    }
  }
}

// ── Conversation panel polling ─────────────────────────────────────────────────
const kStatusColor = {idle:'var(--mut)', listening:'#3fb950', speaking:'var(--acc)'};
async function pollConversation() {
  try {
    const c = await api('/api/conversation');
    $('#convStatus').textContent = c.status||'idle';
    $('#convStatus').style.background = kStatusColor[c.status]||'#21262d';
    $('#convStatus').style.color = '#04101f';
    if (c.transcript) $('#convTranscript').textContent = c.transcript;
    if (c.reply)      $('#convReply').textContent      = c.reply;
  } catch(e) {}
}
setInterval(pollConversation, 800);
pollConversation();

// ── Voice STT (standalone direct path) ────────────────────────────────────────
let _recActive=false, _recCtx, _recProc, _recStream, _recChunks=[];

async function toggleRecord() {
  if (!_recActive) {
    try {
      _recStream = await navigator.mediaDevices.getUserMedia({audio:{channelCount:1},video:false});
      _recCtx = new (window.AudioContext||window.webkitAudioContext)();
      const src = _recCtx.createMediaStreamSource(_recStream);
      _recProc = _recCtx.createScriptProcessor(4096, 1, 1);
      _recChunks = [];
      _recProc.onaudioprocess = e => _recChunks.push(new Float32Array(e.inputBuffer.getChannelData(0)));
      src.connect(_recProc);
      _recProc.connect(_recCtx.destination);
      _recActive = true;
      $('#recBtn').textContent = '■ Stop & Transcribe';
      $('#recBtn').className = 'danger';
      $('#recStatus').textContent = 'recording… click again to stop';
      $('#transcript').textContent = '';
    } catch(e) { flash('mic error: '+e, true); }
  } else {
    _recProc.disconnect();
    _recStream.getTracks().forEach(t => t.stop());
    _recActive = false;
    const sr = _recCtx.sampleRate;
    await _recCtx.close();
    const wav = _buildWav(_recChunks, sr);
    const secs = ((wav.byteLength - 44) / (sr * 2)).toFixed(1);
    $('#recBtn').textContent = '● Record';
    $('#recBtn').className = 'primary';
    $('#recStatus').textContent = `uploading ${secs}s @ ${sr} Hz — waiting for sherpa-onnx…`;
    $('#transcript').textContent = '(QEMU: ~4s model load + 4.5× RT inference — please wait)';
    try {
      const r = await fetch('/api/stt', {method:'POST', body:wav, headers:{'Content-Type':'audio/wav'}});
      const j = await r.json();
      $('#transcript').textContent = j.transcript || '(no speech detected)';
      $('#recStatus').textContent = 'done';
    } catch(e) {
      $('#transcript').textContent = 'error: ' + e;
      $('#recStatus').textContent = 'error';
    }
  }
}

function _buildWav(chunks, rate) {
  let n = 0;
  for (const c of chunks) n += c.length;
  const ab = new ArrayBuffer(44 + n * 2);
  const v = new DataView(ab);
  const ws = (o, t) => { for (let i = 0; i < t.length; i++) v.setUint8(o + i, t.charCodeAt(i)); };
  ws(0, 'RIFF'); v.setUint32(4, 36 + n * 2, true); ws(8, 'WAVE');
  ws(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true);
  v.setUint16(22, 1, true); v.setUint32(24, rate, true); v.setUint32(28, rate * 2, true);
  v.setUint16(32, 2, true); v.setUint16(34, 16, true);
  ws(36, 'data'); v.setUint32(40, n * 2, true);
  let o = 44;
  for (const c of chunks) {
    for (let i = 0; i < c.length; i++) {
      const x = Math.max(-1, Math.min(1, c[i]));
      v.setInt16(o, x < 0 ? x * 32768 : x * 32767, true);
      o += 2;
    }
  }
  return ab;
}
</script>
</body>
</html>
)HTML"

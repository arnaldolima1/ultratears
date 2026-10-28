import './style.css';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { open as openDialog } from '@tauri-apps/plugin-dialog';
import { setLang, currentLang } from './i18n.js';
import { check as checkLauncherUpdate } from '@tauri-apps/plugin-updater';
import { relaunch } from '@tauri-apps/plugin-process';
import { getVersion } from '@tauri-apps/api/app';

const win = getCurrentWindow();
const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];

// ---------------------------------------------------------------- launcher-only preferences (this PC)
const prefs = Object.assign({ music: true, volume: 35, frameless: false, cpus: 0 },
  JSON.parse(localStorage.getItem('prefs') || '{}'));
const savePrefs = () => localStorage.setItem('prefs', JSON.stringify(prefs));

// ---------------------------------------------------------------- artwork
// The launcher's own neutral art (index.html, style.css), or the player's from launcher-art\ (main.rs custom_art):
// bg*.jpg/png as the slideshow, logo.png, loading.jpg as the transition, music.mp3.
let art = { backgrounds: [], logo: null, loading: null, music: null };
let SLIDES = ['n0', 'n1', 'n2', 'n3'];   // neutral slides: CSS classes
async function loadArt() {
  try { art = await invoke('custom_art'); } catch { /* neutral art */ }
  if (art.backgrounds.length) SLIDES = art.backgrounds;
  if (art.logo) $('#logo').innerHTML = `<img src="${art.logo}" alt="" />`;
  if (art.loading) {
    $('#t-img').src = art.loading;
    $('#t-img').hidden = false;
    $('#t-neutral').hidden = true;
  } else $('#transition').classList.add('neutral');
  if (art.music) bgm.src = art.music;
  else $('#music').classList.add('none');
}

// ---------------------------------------------------------------- background slideshow
let slide = 0, slideTimer;
function buildSlides() {
  const bg = $('#bg'), dots = $('#dots');
  SLIDES.forEach((src, i) => {
    const d = document.createElement('div');
    d.className = 'slide';
    if (src.startsWith('data:')) d.style.backgroundImage = `url(${src})`;
    else { d.classList.add(src); d.innerHTML = '<div class="pattern"></div>'; }
    d.style.animationDelay = `${-i * 6}s`;
    bg.append(d);
    const b = document.createElement('button');
    b.title = `Imagem ${i + 1}`;
    b.onclick = () => showSlide(i, true);
    dots.append(b);
  });
  showSlide(0);
}
function showSlide(i, user) {
  slide = (i + SLIDES.length) % SLIDES.length;
  $$('#bg .slide').forEach((s, k) => s.classList.toggle('on', k === slide));
  $$('#dots button').forEach((b, k) => b.classList.toggle('on', k === slide));
  clearTimeout(slideTimer);
  slideTimer = setTimeout(() => showSlide(slide + 1), user ? 14000 : 9000);
}

// ---------------------------------------------------------------- music
const bgm = $('#bgm');
let fadeTimer;
function fadeMusic(to, ms = 800) {
  clearInterval(fadeTimer);
  const from = bgm.volume, t0 = performance.now();
  if (to > 0 && bgm.paused) bgm.play().catch(() => {});
  fadeTimer = setInterval(() => {
    const k = Math.min(1, (performance.now() - t0) / ms);
    bgm.volume = from + (to - from) * k;
    if (k >= 1) { clearInterval(fadeTimer); if (to === 0) bgm.pause(); }
  }, 30);
}
function musicOn() { return prefs.music && !!art.music && !document.body.classList.contains('in-game'); }
function applyMusic() {
  $('#music').classList.toggle('muted', !prefs.music);
  if (musicOn()) fadeMusic(prefs.volume / 100); else fadeMusic(0, 400);
}
// autoplay may wait for the first click
document.addEventListener('pointerdown', () => { if (musicOn() && bgm.paused) applyMusic(); }, { once: true });

// ---------------------------------------------------------------- window buttons and tabs
$('#min').onclick = () => win.minimize();
$('#max').onclick = () => win.toggleMaximize();
$('#close').onclick = () => win.close();
$('#music').onclick = () => { prefs.music = !prefs.music; savePrefs(); $('#set-music').checked = prefs.music; applyMusic(); };

function showView(name) {
  $$('.tab').forEach((t) => t.classList.toggle('active', t.dataset.view === name));
  $$('.view').forEach((v) => v.classList.toggle('active', v.id === `view-${name}`));
  document.body.classList.toggle('panel-open', name !== 'home');
}
$$('.tab').forEach((t) => (t.onclick = () => showView(t.dataset.view)));
$$('[data-set]').forEach((b) => (b.onclick = () => {
  $$('[data-set]').forEach((x) => x.classList.toggle('active', x === b));
  $$('.set-page').forEach((pg) => pg.classList.toggle('active', pg.id === `set-${b.dataset.set}`));
}));

// ---------------------------------------------------------------- status
let status = {};
async function refresh() {
  status = await invoke('status');
  const ver = status.version && status.version.match(/^UltraTears (\S+)/);
  $('#version').textContent = ver ? `v${ver[1]}` : '';
  $('#home-path').value = status.home;
  if (!$('#dump-path').dataset.chosen) $('#dump-path').value = status.dump || '';
  const play = $('#play'), msg = $('#home-status');
  play.disabled = status.building || status.running;
  play.classList.toggle('install', !status.installed);
  play.querySelector('.play-label').textContent = status.installed ? 'Jogar' : 'Instalar';
  if (status.building) msg.innerHTML = 'Instalação em andamento…';
  else if (status.running) msg.innerHTML = 'O jogo está aberto';
  else if (status.installed) msg.innerHTML = `Pronto para jogar · <b>Tears of the Kingdom 1.4.3</b>`;
  else if (!status.can_build) msg.innerHTML = 'Escolha a pasta do jogo em <b>Instalação</b>';
  else msg.innerHTML = 'Jogo ainda não instalado · leva cerca de 30 min';
  renderInstallSource();
}

// How much the install still needs on the game folder's disk (README.md sizes): the dump while it is there
// (~17 GB), the packed RomFS (~17 GB), textures, generated code, objects and the game (~13 GB).
function renderInstallSource() {
  const need = status.installed ? 0 : (status.packed ? 14 : 31) + (status.dump_ready || status.packed ? 0 : 17);
  const free = status.free_gb;
  const el = $('#home-free');
  el.textContent = free == null ? '' : `· ${free.toFixed(0)} GB livres${need ? ` (precisa de ~${need} GB)` : ''}`;
  el.classList.toggle('low', free != null && free < need);
  const ext = installSource();
  $('#dump-done').classList.toggle('hidden', !status.packed);
  $('#dump-manual').classList.toggle('hidden', status.packed || !!ext);
  $('#ext-sources').classList.toggle('hidden', status.packed || !ext);
  $('#use-ext').classList.toggle('hidden', !extensions.some(usableSource));
  $('#clean-row').classList.toggle('hidden', status.packed && !status.temp_dump);
  $('#clean-dump').checked = prefs.cleanDump !== false;
}

// ---------------------------------------------------------------- settings (settings.cfg)
const KNOWN = ['TOTK_FPS_OVERLAY', 'TOTK_FULLSCREEN', 'TOTK_GYRO_AXES', 'TOTK_FPS60', 'TOTK_RENDER_RES', 'TOTK_RUMBLE', 'TOTK_GYRO',
  'TOTK_GYRO_DEADZONE', 'TOTK_OPERATION_MODE'];
const isKnown = (k) => KNOWN.includes(k) || k.startsWith('TOTK_KEY_') || MOUSE_KEYS.includes(k);
function showResField() { $('#res-field').classList.toggle('on', $('#set-resmod').checked); }
let settings = {};
async function loadSettings() {
  settings = await invoke('read_settings');
  $('#set-fps').checked = settings.TOTK_FPS_OVERLAY === '1';
  $('#set-fullscreen').checked = settings.TOTK_FULLSCREEN === '1';
  // controls: rumble and gyro are on unless "0" (host/hid_feedback.c); the dead zone empty = the controller's default
  $('#set-rumble').checked = settings.TOTK_RUMBLE !== '0';
  $('#set-gyro-on').checked = settings.TOTK_GYRO !== '0';
  $('#set-handheld').checked = settings.TOTK_OPERATION_MODE === 'handheld';
  $('#set-gyro').value = settings.TOTK_GYRO_AXES || '';
  $('#set-gyro-dz').value = settings.TOTK_GYRO_DEADZONE || 0;
  $('#set-gyro-dz').dataset.auto = settings.TOTK_GYRO_DEADZONE ? '' : '1';
  showGyro();
  loadInput();
  $('#set-fps60').checked = settings.TOTK_FPS60 === '1';
  $('#mod-fps60').classList.toggle('on', $('#set-fps60').checked);
  $('#set-resmod').checked = !!settings.TOTK_RENDER_RES;
  if (settings.TOTK_RENDER_RES) $('#set-res').value = settings.TOTK_RENDER_RES;
  showResField();
  $('#set-extra').value = Object.entries(settings).filter(([k]) => !isKnown(k)).map(([k, v]) => `${k}=${v}`).join('\n');
  $('#set-borderless').checked = prefs.frameless;
  $('#set-music').checked = prefs.music;
  $('#set-volume').value = prefs.volume;
}
let savedTimer;
async function saveSettings() {
  const s = {
    TOTK_FPS_OVERLAY: $('#set-fps').checked ? '1' : '',
    TOTK_FULLSCREEN: $('#set-fullscreen').checked ? '1' : '',
    TOTK_GYRO_AXES: $('#set-gyro').value.trim(),
    TOTK_RUMBLE: $('#set-rumble').checked ? '' : '0',
    TOTK_GYRO: $('#set-gyro-on').checked ? '' : '0',
    TOTK_GYRO_DEADZONE: $('#set-gyro-dz').dataset.auto ? '' : String(+$('#set-gyro-dz').value),
    TOTK_OPERATION_MODE: $('#set-handheld').checked ? 'handheld' : '',
    TOTK_FPS60: $('#set-fps60').checked ? '1' : '',
    TOTK_RENDER_RES: $('#set-resmod').checked ? $('#set-res').value : '',
    ...inputSettings(),
  };
  for (const line of $('#set-extra').value.split('\n')) {
    const m = line.trim().match(/^(TOTK_[A-Z0-9_]+)\s*=\s*(.*)$/);
    if (m) s[m[1]] = m[2];
  }
  try {
    await invoke('write_settings', { settings: s });
    settings = s;
    $('#saved').classList.add('on');
    clearTimeout(savedTimer);
    savedTimer = setTimeout(() => $('#saved').classList.remove('on'), 1400);
  } catch (e) { showError('Não foi possível salvar', String(e)); }
}
['#set-fps', '#set-fullscreen', '#set-fps60', '#set-rumble', '#set-handheld'].forEach((s) => ($(s).onchange = saveSettings));
// Gyro: its options only matter with it on; the axes field follows the preset (or is typed: "custom")
function showGyro() {
  $('#set-control').classList.toggle('disabled-gyro', !$('#set-gyro-on').checked);
  const dz = $('#set-gyro-dz');
  $('#gyro-dz-val').textContent = dz.dataset.auto ? 'automática' : `${(+dz.value).toFixed(1)} °/s`;
  const v = $('#set-gyro').value.trim(), preset = $('#set-gyro-preset');
  preset.value = [...preset.options].some((o) => o.value === v && v !== 'custom') ? v : 'custom';
  $('#set-gyro').classList.toggle('hidden', preset.value !== 'custom');
}
$('#set-gyro-on').onchange = () => { showGyro(); saveSettings(); };
$('#set-gyro-dz').oninput = (e) => { e.target.dataset.auto = ''; showGyro(); };
$('#set-gyro-dz').onchange = saveSettings;
$('#gyro-dz-auto').onclick = () => { $('#set-gyro-dz').dataset.auto = '1'; $('#set-gyro-dz').value = 0; showGyro(); saveSettings(); };
$('#set-gyro-preset').onchange = (e) => {
  if (e.target.value !== 'custom') $('#set-gyro').value = e.target.value;
  $('#set-gyro').classList.toggle('hidden', e.target.value !== 'custom');
  if (e.target.value === 'custom') $('#set-gyro').focus(); else saveSettings();
};
['#set-gyro', '#set-extra'].forEach((s) => ($(s).onchange = saveSettings));

// Keyboard and mouse (host/keyboard_input.c, host/mouse_camera.c: upstream PR #20). Bindings are SDL scancodes, two per
// action, 512-514 the mouse buttons (src/host/keyboard_bindings.def; only those differing from it are saved).
const MOUSE_KEYS = ['TOTK_CAMERA_MODE', 'TOTK_MOUSE_CAMERA_VERSION', 'TOTK_MOUSE_BACKEND', 'TOTK_MOUSE_LOOK_DEG_PER_COUNT',
  'TOTK_MOUSE_AIM_MULTIPLIER', 'TOTK_MOUSE_SCOPE_MULTIPLIER', 'TOTK_MOUSE_SMOOTH_MS', 'TOTK_MOUSE_INVERT_Y', 'TOTK_MOUSE_SENSITIVITY'];
const ACTIONS = [   // [id, group, name, what it does, default 1, default 2]
  ['MOVE_UP', 'Movimento', 'Frente', 'Analógico esquerdo para cima', 26, 0],
  ['MOVE_DOWN', 'Movimento', 'Trás', 'Analógico esquerdo para baixo', 22, 0],
  ['MOVE_LEFT', 'Movimento', 'Esquerda', 'Analógico esquerdo para a esquerda', 4, 0],
  ['MOVE_RIGHT', 'Movimento', 'Direita', 'Analógico esquerdo para a direita', 7, 0],
  ['LOOK_UP', 'Câmera', 'Olhar para cima', 'Também move a luneta e seleções', 82, 0],
  ['LOOK_DOWN', 'Câmera', 'Olhar para baixo', '', 81, 0],
  ['LOOK_LEFT', 'Câmera', 'Olhar à esquerda', '', 80, 0],
  ['LOOK_RIGHT', 'Câmera', 'Olhar à direita', '', 79, 0],
  ['A', 'Ações', 'A · Interagir', 'Pegar, falar, confirmar', 8, 0],
  ['B', 'Ações', 'B · Correr / cancelar', 'Correr (segurar), guardar a arma', 225, 41],
  ['X', 'Ações', 'X · Pular', 'Parapente no ar', 44, 0],
  ['Y', 'Ações', 'Y · Atacar', 'Ataque carregado (segurar)', 512, 9],
  ['L', 'Ações', 'L · Habilidade', 'Roda de habilidades (segurar)', 6, 0],
  ['R', 'Ações', 'R · Arremessar', 'Mira o arremesso, solta para jogar', 23, 0],
  ['ZL', 'Ações', 'ZL · Focar / escudo', 'Travar no alvo, erguer o escudo', 514, 20],
  ['ZR', 'Ações', 'ZR · Arco', 'Puxar (segurar), soltar dispara', 513, 21],
  ['STICK_L', 'Ações', 'Agachar', 'Clique do analógico esquerdo', 224, 0],
  ['STICK_R', 'Ações', 'Luneta', 'Clique do analógico direito', 226, 0],
  ['PLUS', 'Menus', '+ · Inventário', 'Bolsa e pausa', 43, 0],
  ['MINUS', 'Menus', '− · Purah Pad', 'Mapa e diário', 53, 16],
  ['DPAD_UP', 'Menus rápidos', 'Materiais', 'Direcional para cima (segurar)', 25, 0],
  ['DPAD_DOWN', 'Menus rápidos', 'Assobiar', 'Chama o cavalo', 10, 0],
  ['DPAD_LEFT', 'Menus rápidos', 'Escudos', 'Direcional para a esquerda (segurar)', 29, 0],
  ['DPAD_RIGHT', 'Menus rápidos', 'Armas / arcos', 'Direcional para a direita (segurar)', 27, 0],
];
const RESERVED = new Set([58, 59, 60, 65, 68]);   // F1-F3 (overlays, 60 FPS), F8 (cursor), F11 (fullscreen)
const SDL_CODE = (() => {   // KeyboardEvent.code -> SDL scancode (USB HID usage)
  const m = { Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47, BracketRight: 48,
    Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56, CapsLock: 57, PrintScreen: 70,
    ScrollLock: 71, Pause: 72, Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78, ArrowRight: 79,
    ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86,
    NumpadAdd: 87, NumpadEnter: 88, Numpad0: 98, NumpadDecimal: 99, IntlBackslash: 100, ControlLeft: 224, ShiftLeft: 225,
    AltLeft: 226, MetaLeft: 227, ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231 };
  for (let i = 0; i < 26; i++) m['Key' + String.fromCharCode(65 + i)] = 4 + i;
  for (let i = 1; i <= 9; i++) { m['Digit' + i] = 29 + i; m['Numpad' + i] = 88 + i; }
  m.Digit0 = 39;
  for (let i = 1; i <= 12; i++) m['F' + i] = 57 + i;
  return m;
})();
const MAC = navigator.platform.startsWith('Mac');
function keyName(code) {
  if (!code) return '—';
  if (code === 512) return 'Mouse esq.';
  if (code === 513) return 'Mouse dir.';
  if (code === 514) return 'Mouse meio';
  const named = { 40: MAC ? 'Return' : 'Enter', 41: 'Esc', 42: 'Backspace', 43: 'Tab', 44: 'Espaço', 57: 'Caps Lock', 79: '→', 80: '←',
    81: '↓', 82: '↑', 224: 'Ctrl esq.', 225: 'Shift esq.', 226: MAC ? 'Option esq.' : 'Alt esq.', 227: MAC ? 'Cmd esq.' : 'Win esq.',
    228: 'Ctrl dir.', 229: 'Shift dir.', 230: MAC ? 'Option dir.' : 'Alt dir.', 231: MAC ? 'Cmd dir.' : 'Win dir.', 53: '`' };
  if (named[code]) return named[code];
  if (code >= 4 && code <= 29) return String.fromCharCode(61 + code);
  if (code >= 30 && code <= 39) return String((code - 29) % 10);
  if (code >= 58 && code <= 69) return 'F' + (code - 57);
  const back = Object.entries(SDL_CODE).find(([, v]) => v === code);
  return back ? back[0].replace(/^Numpad/, 'Num ') : `Tecla ${code}`;
}
let binds = {};   // id -> [code, code]
let listening = null;   // {id, slot, el}
function loadInput() {
  $('#set-cam').value = settings.TOTK_CAMERA_MODE === 'mouse' ? 'mouse' : '';
  $('#set-mouse-sens').value = settings.TOTK_MOUSE_LOOK_DEG_PER_COUNT || 0.04;
  $('#set-mouse-aim').value = settings.TOTK_MOUSE_AIM_MULTIPLIER || 1;
  $('#set-mouse-scope').value = settings.TOTK_MOUSE_SCOPE_MULTIPLIER || 1;
  $('#set-mouse-smooth').value = ['5', '10'].includes(settings.TOTK_MOUSE_SMOOTH_MS) ? settings.TOTK_MOUSE_SMOOTH_MS : '0';
  $('#set-mouse-invert').checked = settings.TOTK_MOUSE_INVERT_Y === '1';
  binds = {};
  for (const [id, , , , d1, d2] of ACTIONS) {
    const m = (settings['TOTK_KEY_' + id] || '').match(/^(\d+),(\d+)$/);
    binds[id] = m ? [+m[1], +m[2]] : [d1, d2];
  }
  showInput();
}
function inputSettings() {
  const mouse = $('#set-cam').value === 'mouse', s = {};
  s.TOTK_CAMERA_MODE = mouse ? 'mouse' : '';
  s.TOTK_MOUSE_CAMERA_VERSION = mouse ? '2' : '';
  const num = (sel, def) => (+$(sel).value === def ? '' : String(+$(sel).value));
  s.TOTK_MOUSE_LOOK_DEG_PER_COUNT = num('#set-mouse-sens', 0.04);
  s.TOTK_MOUSE_AIM_MULTIPLIER = num('#set-mouse-aim', 1);
  s.TOTK_MOUSE_SCOPE_MULTIPLIER = num('#set-mouse-scope', 1);
  s.TOTK_MOUSE_SMOOTH_MS = $('#set-mouse-smooth').value === '0' ? '' : $('#set-mouse-smooth').value;
  s.TOTK_MOUSE_INVERT_Y = $('#set-mouse-invert').checked ? '1' : '';
  for (const [id, , , , d1, d2] of ACTIONS) {
    const [a, b] = binds[id];
    s['TOTK_KEY_' + id] = a === d1 && b === d2 ? '' : `${a},${b}`;
  }
  return s;
}
function showInput() {
  $('#set-control').classList.toggle('keys-camera', $('#set-cam').value !== 'mouse');
  $('#mouse-sens-val').textContent = `${(+$('#set-mouse-sens').value).toFixed(3)} °/ponto`;
  $('#mouse-aim-val').textContent = `${(+$('#set-mouse-aim').value).toFixed(2)}×`;
  $('#mouse-scope-val').textContent = `${(+$('#set-mouse-scope').value).toFixed(2)}×`;
  const box = $('#bindings');
  box.textContent = '';
  let group = '';
  for (const [id, g, name, what] of ACTIONS) {
    if (g !== group) { group = g; const h = document.createElement('h4'); h.textContent = g; box.append(h); }
    const row = document.createElement('div');
    row.className = 'bind-row';
    const label = document.createElement('div');
    label.className = 'what';
    label.textContent = name;
    if (what) { const sm = document.createElement('small'); sm.textContent = what; label.append(sm); }
    row.append(label);
    binds[id].forEach((code, slot) => {
      const el = document.createElement('div');
      el.className = 'bind-slot' + (code ? '' : ' empty');
      el.tabIndex = 0;
      el.textContent = keyName(code);
      const x = document.createElement('button');
      x.className = 'clear';
      x.title = 'Apagar';
      x.textContent = '×';
      x.onmousedown = (e) => e.stopPropagation();
      x.onclick = (e) => { e.stopPropagation(); stopListening(); binds[id][slot] = 0; showInput(); saveSettings(); };
      el.append(x);
      el.onmousedown = (e) => {
        if (listening && listening.el === el) { e.preventDefault(); assign([512, 514, 513][e.button]); return; }
        stopListening();
        listening = { id, slot, el };
        el.classList.add('listening');
        el.firstChild.textContent = 'Aperte…';
        e.preventDefault();
      };
      el.oncontextmenu = (e) => e.preventDefault();
      row.append(el);
    });
    box.append(row);
  }
}
function stopListening() {
  if (!listening) return;
  listening.el.classList.remove('listening');
  listening = null;
  showInput();
}
function assign(code) {
  if (!listening || code === undefined) return;
  const { id, slot } = listening;
  for (const k in binds) binds[k] = binds[k].map((c) => (c === code ? 0 : c));   // one action per key (as the game does)
  binds[id][slot] = code;
  listening = null;
  showInput();
  saveSettings();
}
addEventListener('keydown', (e) => {
  if (!listening) return;
  e.preventDefault();
  e.stopPropagation();
  const code = SDL_CODE[e.code];
  if (code && !RESERVED.has(code)) assign(code);
}, true);
addEventListener('mousedown', (e) => { if (listening && !listening.el.contains(e.target)) stopListening(); });
$('#keys-reset').onclick = () => {
  stopListening();
  for (const [id, , , , d1, d2] of ACTIONS) binds[id] = [d1, d2];
  showInput();
  saveSettings();
};
$('#set-cam').onchange = () => { showInput(); saveSettings(); };
['#set-mouse-sens', '#set-mouse-aim', '#set-mouse-scope'].forEach((sel) => { $(sel).oninput = showInput; $(sel).onchange = saveSettings; });
['#set-mouse-smooth', '#set-mouse-invert'].forEach((sel) => ($(sel).onchange = saveSettings));
// The resolution mod: the heap overlay for the chosen size is made first (tools/make_res_overlay.py), then saved.
async function applyRes() {
  showResField();
  const status = $('#res-status');
  if ($('#set-resmod').checked) {
    status.textContent = 'Preparando a memória do jogo para essa resolução…';
    try {
      await invoke('make_res_overlay', { size: $('#set-res').value });
      status.textContent = 'Pronto: vale na próxima vez que o jogo abrir.';
    } catch (e) {
      $('#set-resmod').checked = false;
      showResField();
      status.textContent = '';
      showError('Não foi possível preparar a resolução', String(e));
    }
  } else status.textContent = '';
  await saveSettings();
}
$('#set-resmod').onchange = applyRes;
$('#set-res').onchange = applyRes;
$('#set-borderless').onchange = (e) => { prefs.frameless = e.target.checked; savePrefs(); };
$('#set-music').onchange = (e) => { prefs.music = e.target.checked; savePrefs(); applyMusic(); };
$('#set-volume').oninput = (e) => { prefs.volume = +e.target.value; savePrefs(); if (musicOn()) bgm.volume = prefs.volume / 100; };
$$('[data-url]').forEach((b) => (b.onclick = () => invoke('open_path', { path: b.dataset.url })));
$$('[data-open]').forEach((b) => (b.onclick = () => invoke('open_path', { path: b.dataset.open }).catch((e) => showError('Não foi possível abrir', String(e)))));

// ---------------------------------------------------------------- dialog
function showError(title, body) {
  $('#dialog-title').textContent = title;
  $('#dialog-body').textContent = body;
  $('#dialog').classList.remove('hidden');
}
$('#dialog-ok').onclick = () => $('#dialog').classList.add('hidden');
$('#dialog-log').onclick = () => invoke('open_path', { path: 'lasrecomp.log' });

// ---------------------------------------------------------------- build
// Steps of tools/setup.py with their usual share of the time (8-core PC, README.md).
// "extract" (a dump-source extension) only when the install starts with it: useSteps().
const EXTRACT = ['extract', 'Extrair', 'NSP / XCI → dump', 6];
const SOURCE = ['source', 'Baixar', 'código do jogo', 1];   // (game_update.rs: a new folder, or an update)
const BUILD_STEPS = [
  ['check', 'Verificar', 'versão do dump', 1],
  ['romfs', 'RomFS', 'empacotar arquivos', 4],
  ['lift', 'Traduzir', 'código → C', 2],
  ['compile', 'Compilar', 'gerar o .exe', 15],
  ['shaders', 'Shaders', 'para Vulkan', 5],
  ['textures', 'Texturas', 'BC7 / BC1', 7],
  ['finish', 'Concluir', 'configuração', 0.5],
];
let STEPS = BUILD_STEPS, TOTAL = 0;
function useSteps(extract, source = false) {
  STEPS = [...(source ? [SOURCE] : []), ...(extract ? [EXTRACT] : []), ...BUILD_STEPS];
  TOTAL = STEPS.reduce((a, s) => a + s[3], 0);
  build.state = STEPS.map(() => '');
  buildSteps();
}
const build = { step: -1, sub: 0, start: 0, stepStart: 0, state: [], running: false };

function buildSteps() {
  $('#steps').innerHTML = STEPS.map(([id, name, what]) =>
    `<li data-step="${id}"><b>${name}</b>${what}<span class="sub"><i></i></span></li>`).join('');
}
function renderBuild() {
  STEPS.forEach((s, i) => {
    const li = $(`#steps li[data-step="${s[0]}"]`);
    li.className = build.state[i];
    li.querySelector('.sub i').style.width = i === build.step && build.state[i] === 'run' ? `${build.sub * 100}%` : '';
  });
  let done = 0;
  STEPS.forEach((s, i) => { if (build.state[i] === 'done') done += s[3]; });
  if (build.step >= 0 && build.state[build.step] === 'run') done += STEPS[build.step][3] * build.sub;
  const pct = Math.min(100, (done / TOTAL) * 100);
  $('#bar-fill').style.width = `${pct}%`;
  $('#build-pct').textContent = `${Math.floor(pct)}%`;
  if (build.running && pct > 2) {
    const el = (Date.now() - build.start) / 1000, left = el * (100 - pct) / pct;
    $('#build-eta').textContent = left > 60 ? `~${Math.round(left / 60)} min restantes` : '< 1 min';
  } else $('#build-eta').textContent = '';
}
function setBuilding(on) {
  build.running = on;
  $('#build').disabled = on;
  $('#cancel').disabled = !on;
  $('.bar').classList.toggle('idle', !on);
  refresh();
}

const logEl = $('#log');
function logLine(text, cls) {
  const span = document.createElement('span');
  if (cls) span.className = cls;
  span.textContent = text + '\n';
  logEl.append(span);
  while (logEl.childNodes.length > 800) logEl.firstChild.remove();
  logEl.scrollTop = logEl.scrollHeight;
}

function onBuildLine({ text, err }) {
  const t = text.trimEnd();
  const stepLine = t.match(/^\[(\d)\/7\] (\w+)$/);
  if (stepLine) {
    const i = STEPS.findIndex((s) => s[0] === stepLine[2]);
    for (let k = 0; k < i; k++) if (build.state[k] !== 'done') build.state[k] = 'done';
    build.step = i; build.sub = 0; build.state[i] = 'run'; build.stepStart = Date.now();
    if (STEPS.findIndex((s) => s[0] === 'lift') <= i) cleanTempDump();   // the RomFS is packed: the dump is not read any more
    $('#build-phase').textContent = `${STEPS[i][1]} · ${STEPS[i][2]}`;
    logLine(t, 'step');
    renderBuild();
    return;
  }
  if (/^\s{2}\S.*\(\d+ s\)$/.test(t) && build.step >= 0) {   // "  result (N s)": the step is over
    build.state[build.step] = 'done'; build.sub = 1;
    logLine(t, 'step');
    renderBuild();
    return;
  }
  if (/^ERROR/.test(t.trim())) {
    if (build.step >= 0) build.state[build.step] = 'fail';
    $('.bar').classList.add('error');
    $('#build-phase').textContent = 'Erro';
  }
  // progress inside a step: "[n/m]" (ninja), "n/m", "NN%"
  let m = t.match(/\[(\d+)\/(\d+)\]/) || t.match(/\b(\d+)\s*\/\s*(\d+)\b/);
  if (m && +m[2] >= 10 && +m[1] <= +m[2]) build.sub = Math.max(build.sub, +m[1] / +m[2]);
  else if ((m = t.match(/(\d+(?:\.\d+)?)\s*%/)) && +m[1] <= 100) build.sub = Math.max(build.sub, +m[1] / 100);
  if (t.trim()) $('#build-activity').textContent = t.trim().replace(/^\[\d+\/\d+\]\s*/, '').slice(0, 160);
  logLine(t, err ? 'err' : '');
  renderBuild();
}

async function startBuild(afterExtract = false) {
  if (!afterExtract) { useSteps(false); build.start = Date.now(); }
  build.step = -1; build.sub = 0;
  $('.bar').classList.remove('error');
  $('#build-phase').textContent = 'Iniciando…';
  renderBuild();
  try {
    await invoke('start_build', { dump: $('#dump-path').dataset.chosen || null, cpus: +$('#cpus').value, only: null });
    setBuilding(true);
  } catch (e) { showError('Não foi possível iniciar a instalação', String(e)); }
}
$('#build').onclick = () => install();
$('#clean-dump').onchange = (e) => { prefs.cleanDump = e.target.checked; savePrefs(); };
$('#cancel').onclick = () => invoke('cancel_build');
$('#cpus').oninput = (e) => { $('#cpus-val').textContent = e.target.value; prefs.cpus = +e.target.value; savePrefs(); };

$('#pick-dump').onclick = async () => {
  const dir = await openDialog({ directory: true, title: 'Pasta do dump (com exefs e romfs)' });
  if (dir) { $('#dump-path').value = dir; $('#dump-path').dataset.chosen = dir; }
};
$('#pick-home').onclick = async () => {
  const dir = await openDialog({ directory: true, title: 'Pasta do UltraTears' });
  if (!dir) return;
  try { await invoke('set_home', { path: dir }); await refresh(); await loadSettings(); }
  catch (e) { showError('Pasta inválida', String(e)); }
};

listen('build-line', (e) => onBuildLine(e.payload));
listen('build-done', async (e) => {
  const ok = e.payload === 0;
  if (ok) { build.state = build.state.map((s) => (s === 'run' || s === '' ? 'done' : s)); $('#build-phase').textContent = 'Concluído'; }
  else if (!build.state.includes('fail')) {
    if (build.step >= 0) build.state[build.step] = 'fail';
    $('#build-phase').textContent = e.payload === 1 ? 'Erro' : 'Cancelado';
    $('.bar').classList.add('error');
  }
  build.sub = ok ? 1 : build.sub;
  renderBuild();
  setBuilding(false);
  if (ok) cleanTempDump();
  if (!ok && e.payload === 1) showError('A instalação parou', [...logEl.childNodes].slice(-14).map((n) => n.textContent).join(''));
});

// ---------------------------------------------------------------- extensions
// Installed from a .zip in Configurações (main.rs install_extension). A "dump-source" one gets a block in Instalação:
// the files it asks for, the output folder (default: game/ in the game folder), and "Extrair"; its lines
// "progress <done> <total>" move its bar. Chosen paths are remembered per extension (prefs.ext[id]).
let extensions = [];
let extRun = null;   // { id }: the extraction running, the build follows it
const esc = (t) => String(t).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
const baseName = (p) => p.split(/[\\/]/).pop();
prefs.ext = prefs.ext || {};

async function loadExtensions() {
  try { extensions = await invoke('list_extensions'); } catch { extensions = []; }
  $('#ext-list').innerHTML = extensions.length ? extensions.map((e) => `<li><div><b>${esc(e.name)}</b> <small>${esc(e.version)} · ${esc(e.id)}${e.supported ? '' : ' · sem programa para este sistema'}</small><small>${esc(e.description)}</small></div><button class="ghost" data-ext-remove="${esc(e.id)}">Remover</button></li>`).join('')
    : '<li class="empty">Nenhuma extensão instalada.</li>';
  $$('[data-ext-remove]').forEach((b) => (b.onclick = async () => {
    try { await invoke('remove_extension', { id: b.dataset.extRemove }); } catch (e) { showError('Não foi possível remover', String(e)); }
    loadExtensions();
  }));
  renderExtSources();
}

const usableSource = (e) => e.kind === 'dump-source' && e.supported;
// The extension that makes the dump, unless the player chose a folder already extracted (prefs.source = 'folder')
function installSource() {
  return prefs.source === 'folder' ? null : extensions.find(usableSource) || null;
}

function renderExtSources() {
  const e = installSource();
  if (!e) { $('#ext-sources').innerHTML = ''; if (status.home) renderInstallSource(); return; }
  const chosen = prefs.ext[e.id] || {};
  const fields = e.inputs.map((i) => {
    const v = chosen[i.key] || [];
    const shown = v.length > 1 ? `${v.length} arquivos: ${v.map(baseName).join(', ')}` : (v[0] || '');
    return `<label class="field"><span>${esc(i.label)}</span><div class="path"><input readonly value="${esc(shown)}" title="${esc(v.join('\n'))}" /><button class="ghost" data-ext-pick="${esc(i.key)}">Escolher</button></div></label>`;
  }).join('');
  $('#ext-sources').innerHTML = `<div class="ext-source" data-ext="${esc(e.id)}">
    <h3>${esc(e.name)} <button class="link" id="use-folder">já tenho o exefs e o romfs extraídos</button></h3>
    <p class="hint">${esc(e.description)} O dump vai para <code>game/</code> na pasta do jogo; Instalar extrai e já compila em seguida.</p>
    <div class="grid2">${fields}</div>
  </div>`;
  $$('[data-ext-pick]').forEach((b) => (b.onclick = async () => {
    const i = e.inputs.find((x) => x.key === b.dataset.extPick);
    const dir = i.type === 'dir';
    const got = await openDialog({ title: i.label, multiple: i.type === 'files', directory: dir,
      filters: !dir && i.extensions.length ? [{ name: i.label, extensions: i.extensions }] : undefined });
    if (!got) return;
    prefs.ext[e.id] = Object.assign(prefs.ext[e.id] || {}, { [i.key]: Array.isArray(got) ? got : [got] });
    savePrefs();
    renderExtSources();
  }));
  $('#use-folder').onclick = () => { prefs.source = 'folder'; savePrefs(); renderExtSources(); };
  if (status.home) renderInstallSource();
}
$('#use-ext').onclick = () => { prefs.source = 'ext'; savePrefs(); renderExtSources(); };

// Instalar: the build straight away when the game is already packed or a dump is there; otherwise the extension
// makes the dump first (then the build follows from ext-done).
// ---------------------------------------------------------------- updates
// The launcher's own (tauri-plugin-updater: the GitHub release's latest.json, signed) and the game's (game_update.rs:
// newer commits of the branch than the folder's VERSION.txt). Checked once at start; each shows a card on Início.
let gameUpdate = null, launcherUpdate = null;
async function checkUpdates() {
  try { launcherUpdate = await checkLauncherUpdate(); } catch { launcherUpdate = null; }   // (offline, or no release yet)
  if (status.installed && !status.dev) {
    try { const u = await invoke('game_update_check'); gameUpdate = u.available ? u : null; } catch { gameUpdate = null; }
  }
  renderUpdates();
}
function renderUpdates() {
  const box = $('#updates');
  box.textContent = '';
  const card = (title, sub, items, label, action) => {
    const c = document.createElement('div');
    c.className = 'update-card';
    const head = document.createElement('div');
    head.className = 'update-head';
    const t = document.createElement('div');
    const b = document.createElement('b'); b.textContent = title;
    const sm = document.createElement('small'); sm.textContent = sub;
    t.append(b, sm);
    const btn = document.createElement('button');
    btn.className = 'gold';
    btn.textContent = label;
    btn.onclick = () => action(btn);
    head.append(t, btn);
    c.append(head);
    if (items.length) {
      const ul = document.createElement('ul');
      items.slice(0, 4).forEach((x) => { const li = document.createElement('li'); li.textContent = x; ul.append(li); });
      if (items.length > 4) { const li = document.createElement('li'); li.className = 'more'; li.textContent = `+ ${items.length - 4} mudanças`; ul.append(li); }
      c.append(ul);
    }
    box.append(c);
  };
  if (gameUpdate) {
    const n = gameUpdate.ahead || gameUpdate.commits.length;
    card('Atualização do jogo', n === 1 ? '1 mudança nova' : `${n} mudanças novas`, gameUpdate.commits, 'Atualizar',
      () => { if (!status.building) install(gameUpdate); });
  }
  if (launcherUpdate) {
    const notes = (launcherUpdate.body || '').split('\n').map((l) => l.replace(/^[-*]\s*/, '').trim()).filter(Boolean);
    card(`Launcher ${launcherUpdate.version}`, 'Nova versão do launcher', notes, 'Atualizar launcher', async (btn) => {
      btn.disabled = true;
      let got = 0, size = 0;
      try {
        await launcherUpdate.downloadAndInstall((ev) => {
          if (ev.event === 'Started') size = ev.data.contentLength || 0;
          if (ev.event === 'Progress') { got += ev.data.chunkLength; btn.textContent = size ? `${Math.round((got / size) * 100)}%` : '…'; }
        });
        await relaunch();
      } catch (e) { btn.disabled = false; btn.textContent = 'Atualizar launcher'; showError('Não foi possível atualizar o launcher', String(e)); }
    });
  }
}

// The game's source first when the folder has none yet or an update was asked for (game_update.rs), then the
// extension's extraction when the dump is still to be made, then the build.
async function install(update = null) {
  await refresh();
  const e = installSource();
  // (with the extension: only a dump it finished, TEMP_MARK; a partial one is resumed by running it again)
  const extract = !(status.packed || (e ? status.temp_dump : true));
  const chosen = e ? prefs.ext[e.id] || {} : {};
  if (extract) {
    const missing = e.inputs.find((i) => !(chosen[i.key] || []).length);
    if (missing) return showError('Falta escolher', missing.label);
  }
  const source = !status.dev && (!!update || !status.tools);
  useSteps(extract, source);
  build.start = Date.now();
  $('.bar').classList.remove('error');
  logEl.textContent = '';
  showView('install');
  if (source && !(await fetchSource(update && update.remote))) return;
  if (!extract) return startBuild(true);
  const xi = STEPS.indexOf(EXTRACT);
  build.stepStart = Date.now(); build.step = xi; build.sub = 0; build.state[xi] = 'run';
  $('#build-phase').textContent = `${EXTRACT[1]} · ${e.name}`;
  $('#build-activity').textContent = 'Lendo os pacotes…';
  renderBuild();
  try {
    await invoke('run_extension', { id: e.id, inputs: Object.fromEntries(e.inputs.map((i) => [i.key, chosen[i.key]])), out: '' });
    extRun = { id: e.id };
    setBuilding(true);
  } catch (err) { showError('Não foi possível iniciar a extração', String(err)); }
}

// Downloads the source of commit `sha` (default: the newest of the branch) over the game folder. true when done.
let sourceWait = null;
async function fetchSource(sha) {
  const si = STEPS.indexOf(SOURCE);
  build.stepStart = Date.now(); build.step = si; build.sub = 0; build.state[si] = 'run';
  $('#build-phase').textContent = `${SOURCE[1]} · ${SOURCE[2]}`;
  $('#build-activity').textContent = 'Consultando o GitHub…';
  renderBuild();
  setBuilding(true);
  try {
    if (!sha) sha = (await invoke('game_update_check')).remote;
    const done = new Promise((resolve) => (sourceWait = resolve));
    await invoke('game_update_apply', { sha });
    const err = await done;
    if (err) throw err;
    build.state[si] = 'done'; build.sub = 1;
    logLine(`Código do jogo: commit ${sha.slice(0, 7)}`, 'step');
    renderBuild();
    gameUpdate = null;
    renderUpdates();
    return true;
  } catch (err) {
    build.state[si] = 'fail';
    $('.bar').classList.add('error');
    $('#build-phase').textContent = 'Erro';
    renderBuild();
    showError('Não foi possível baixar o jogo', String(err));
    return false;
  } finally {
    setBuilding(false);
  }
}
listen('update-progress', ({ payload: { text, done, total } }) => {
  $('#build-activity').textContent = done ? `${text} · ${(done / 1e6).toFixed(1)}${total ? ` de ${(total / 1e6).toFixed(1)}` : ''} MB` : text;
  if (total) { build.sub = Math.min(1, done / total); renderBuild(); }
});
listen('update-done', ({ payload }) => { if (sourceWait) { sourceWait(payload); sourceWait = null; } });

let cleaning = false;
async function cleanTempDump() {
  if (prefs.cleanDump === false || cleaning || !status.temp_dump) return;
  cleaning = true;
  try {
    const gb = await invoke('remove_temp_dump');
    if (gb) logLine(`Dump temporário apagado: ${gb.toFixed(1)} GB liberados`, 'step');
    await refresh();
  } catch { /* not packed yet: next step */ }
  cleaning = false;
}

listen('ext-line', ({ payload: { text, err } }) => {
  const m = text.match(/^progress (\d+) (\d+)$/);
  if (m) {
    const done = +m[1], total = +m[2], secs = (Date.now() - build.stepStart) / 1000;
    build.sub = total ? Math.min(1, done / total) : 0;
    $('#build-activity').textContent = `${(done / 1e9).toFixed(1)} de ${(total / 1e9).toFixed(1)} GB` +
      (secs > 3 ? ` · ${(done / 1e6 / secs).toFixed(0)} MB/s` : '');
    renderBuild();
    return;
  }
  logLine(text, err ? 'err' : '');
  if (text.trim()) $('#build-activity').textContent = text.trim().slice(0, 160);
});
listen('ext-done', async ({ payload: code }) => {
  extRun = null;
  setBuilding(false);
  const xi = STEPS.indexOf(EXTRACT);
  if (code !== 0) {
    build.state[xi] = 'fail';
    $('.bar').classList.add('error');
    $('#build-phase').textContent = code === 1 ? 'Erro na extração' : 'Cancelado';
    renderBuild();
    if (code === 1) showError('A extração parou', [...logEl.childNodes].slice(-14).map((n) => n.textContent).join(''));
    return;
  }
  build.state[xi] = 'done'; build.sub = 1;
  renderBuild();
  startBuild(true);
});

$('#ext-add').onclick = async () => {
  const zip = await openDialog({ title: 'Extensão do launcher', filters: [{ name: 'Extensão', extensions: ['zip'] }] });
  if (!zip) return;
  try { const e = await invoke('install_extension', { zip }); showError('Extensão adicionada', `${e.name} ${e.version}`); }
  catch (e) { showError('Não foi possível adicionar a extensão', String(e)); }
  loadExtensions();
};

// ---------------------------------------------------------------- play: transition, hand-over, return
async function play() {
  if (!status.installed) { showView('install'); return; }
  const fullscreen = $('#set-fullscreen').checked;
  document.body.classList.add('in-game');
  $('#t-msg').textContent = 'Uma nova jornada começa no céu';
  $('#transition').classList.add('on');
  fadeMusic(0, 1400);
  if (fullscreen) await win.setFullscreen(true);
  await new Promise((r) => setTimeout(r, 1000));   // the transition fades in first
  const pos = await win.outerPosition(), size = await win.outerSize();
  try {
    await invoke('launch_game', {
      rect: [pos.x, pos.y, size.width, size.height], fullscreen, borderless: prefs.frameless, lang: currentLang(),
    });
    watchStartup();
    refresh();
  } catch (e) {
    backFromGame();
    showError('Não foi possível abrir o jogo', String(e));
  }
}
async function backFromGame() {
  document.body.classList.remove('in-game');
  if (await win.isFullscreen()) await win.setFullscreen(false);
  await win.show();
  await win.unminimize();
  await win.setFocus();
  setTimeout(() => $('#transition').classList.remove('on'), 150);
  applyMusic();
  refresh();
}
$('#play').onclick = play;

// The game's start-up under the transition (startup.txt via main.rs startup_status): what it is doing while its window
// stays hidden - preparing, then compiling the shaders it made ahead (n / total) - until it shows ("ready").
let startupTimer;
function watchStartup() {
  const box = $('#t-status'), text = $('#t-status-text'), bar = $('#t-bar');
  const t0 = Date.now();
  clearInterval(startupTimer);
  box.classList.remove('on', 'busy');
  startupTimer = setInterval(async () => {
    let s = '';
    try { s = await invoke('startup_status'); } catch { /* not yet */ }
    const m = s.match(/^shaders (\d+) (\d+)/);
    if (s.startsWith('ready') || !document.body.classList.contains('in-game')) { clearInterval(startupTimer); box.classList.remove('on'); return; }
    if (m && +m[2]) {
      box.classList.add('on'); box.classList.remove('busy');
      const n = Math.min(+m[1], +m[2]);
      text.textContent = `Compilando shaders · ${n.toLocaleString('pt-BR')} / ${(+m[2]).toLocaleString('pt-BR')}`;
      bar.style.width = `${(n / +m[2]) * 100}%`;
    } else if (Date.now() - t0 > 1500) {   // (the tagline alone first)
      box.classList.add('on', 'busy');
      text.textContent = 'Preparando o jogo';
    }
  }, 250);
}
listen('game-visible', async () => {
  await new Promise((r) => setTimeout(r, 250));   // the game's first frame is up behind us
  await win.hide();
});
listen('game-exit', async (e) => {
  await backFromGame();
  if (e.payload !== 0) {
    const tail = await invoke('log_tail', { lines: 18 });
    showError(`O jogo fechou com erro (código ${e.payload})`, tail || 'Sem log.');
  }
});

// ---------------------------------------------------------------- game controller (src-tauri pad_thread)
// The d-pad or left stick moves a highlight to the nearest control in that direction; the bottom button presses it,
// the right one goes back (closes a dialog, returns to Início), Start plays. Sliders and lists change with left/right.
let padEl = null;
const PAD_SEL = 'button:not(:disabled):not(.win-btn), select, input[type=range], input[type=text], input:not([type]), textarea, .bind-slot, label.toggle';
function padVisible(el) {
  if (!el || !el.isConnected) return false;
  const r = el.getBoundingClientRect();
  return r.width > 0 && r.height > 0 && !el.closest('.hidden, #min, #max, #close');
}
function padCandidates() {
  const dialog = $('#dialog');
  const root = !dialog.classList.contains('hidden') ? dialog : document;
  return [...root.querySelectorAll(PAD_SEL)].filter((el) => padVisible(el) && !(root === document && el.closest('#dialog')));
}
function padFocus(el) {
  if (padEl) padEl.classList.remove('pad-focus');
  padEl = el;
  if (!el) return;
  el.classList.add('pad-focus');
  el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}
function padMove(dir) {
  const list = padCandidates();
  if (!padVisible(padEl) || !list.includes(padEl)) return padFocus(list.find((e) => e.id === 'play') || list[0]);
  if (padEl.matches('input[type=range]') && (dir === 'left' || dir === 'right')) {
    if (dir === 'left') padEl.stepDown(); else padEl.stepUp();
    padEl.dispatchEvent(new Event('input')); padEl.dispatchEvent(new Event('change'));
    return;
  }
  if (padEl.matches('select') && (dir === 'left' || dir === 'right')) {
    const i = padEl.selectedIndex + (dir === 'left' ? -1 : 1);
    if (i >= 0 && i < padEl.options.length) { padEl.selectedIndex = i; padEl.dispatchEvent(new Event('change')); }
    return;
  }
  const a = padEl.getBoundingClientRect(), ax = a.left + a.width / 2, ay = a.top + a.height / 2;
  let best = null, bestScore = Infinity;
  for (const el of list) {
    if (el === padEl) continue;
    const b = el.getBoundingClientRect(), dx = b.left + b.width / 2 - ax, dy = b.top + b.height / 2 - ay;
    const along = { up: -dy, down: dy, left: -dx, right: dx }[dir], across = dir === 'up' || dir === 'down' ? Math.abs(dx) : Math.abs(dy);
    if (along <= 4) continue;
    const score = along + across * 2;
    if (score < bestScore) { bestScore = score; best = el; }
  }
  if (best) padFocus(best);
}
function padPress() {
  const el = padEl;
  if (!padVisible(el)) return padMove('down');
  if (el.matches('.bind-slot')) { if (!el.classList.contains('listening')) el.dispatchEvent(new MouseEvent('mousedown', { button: 0, bubbles: true })); return; }
  if (el.matches('input[type=text], input:not([type]), textarea')) { el.focus(); return; }
  if (el.matches('select, input[type=range]')) return;
  el.click();
}
listen('pad', ({ payload }) => {
  if (!document.hasFocus()) return;
  if (!$('#intro').classList.contains('gone')) { dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' })); return; }
  if (payload === 'ok') return padPress();
  if (payload === 'back') {
    if (!$('#dialog').classList.contains('hidden')) return $('#dialog-ok').click();
    if (listening) return stopListening();
    if (document.activeElement && document.activeElement.matches('input, textarea')) return document.activeElement.blur();
    showView('home');
    return padFocus($('#play'));
  }
  if (payload === 'start') {
    showView('home');
    padFocus($('#play'));
    if (!$('#play').disabled) $('#play').click();
    return;
  }
  padMove(payload);
});
addEventListener('mousemove', () => padFocus(null));

// ---------------------------------------------------------------- start
// ---------------------------------------------------------------- opening
// index.html #intro: plays once at start (prefs.intro, Configurações > Launcher); a click or a key skips it.
function playIntro() {
  const el = $('#intro');
  if (prefs.intro === false || matchMedia('(prefers-reduced-motion: reduce)').matches) { el.classList.add('gone'); return; }
  let ended = false;
  const end = () => {
    if (ended) return;
    ended = true;
    el.classList.add('done');
    setTimeout(() => el.classList.add('gone'), 950);
    removeEventListener('keydown', end);
  };
  el.onclick = end;
  addEventListener('keydown', end);
  setTimeout(end, 3600);
}
// Language (i18n.js): the saved choice, else the system's
setLang(prefs.lang || (navigator.language.toLowerCase().startsWith('pt') ? 'pt' : 'en'));
$('#set-lang').value = prefs.lang || (navigator.language.toLowerCase().startsWith('pt') ? 'pt' : 'en');
$('#set-lang').onchange = (e) => { prefs.lang = e.target.value; savePrefs(); setLang(prefs.lang); };
playIntro();
$('#set-intro').checked = prefs.intro !== false;
$('#set-intro').onchange = (e) => { prefs.intro = e.target.checked; savePrefs(); };

(async function init() {
  await loadArt();
  buildSlides();
  useSteps(false);
  renderBuild();
  const n = await invoke('cpu_count');
  $('#cpus').max = n;
  $('#cpus').value = prefs.cpus && prefs.cpus <= n ? prefs.cpus : n;
  $('#cpus-val').textContent = $('#cpus').value;
  await refresh();
  await loadSettings();
  await loadExtensions();
  setBuilding(status.building);
  bgm.volume = 0;
  applyMusic();
  const view = await invoke('start_view');
  if (view && $(`#view-${view}`)) showView(view);
  const lv = await getVersion();
  $('#about-versions').textContent = `Launcher ${lv}` + (status.commit ? ` · jogo: commit ${status.commit}` : '');
  checkUpdates();
})();

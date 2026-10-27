import './style.css';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { open as openDialog } from '@tauri-apps/plugin-dialog';

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
  else if (!status.tools) msg.innerHTML = 'Escolha a pasta do UltraTears em <b>Instalação</b>';
  else msg.innerHTML = 'Jogo ainda não instalado · leva cerca de 30 min';
}

// ---------------------------------------------------------------- settings (settings.cfg)
const KNOWN = ['TOTK_FPS_OVERLAY', 'TOTK_FULLSCREEN', 'TOTK_GYRO_AXES', 'TOTK_FPS60', 'TOTK_RENDER_RES'];
function showResField() { $('#res-field').classList.toggle('on', $('#set-resmod').checked); }
let settings = {};
async function loadSettings() {
  settings = await invoke('read_settings');
  $('#set-fps').checked = settings.TOTK_FPS_OVERLAY === '1';
  $('#set-fullscreen').checked = settings.TOTK_FULLSCREEN === '1';
  $('#set-gyro').value = settings.TOTK_GYRO_AXES || '';
  $('#set-fps60').checked = settings.TOTK_FPS60 === '1';
  $('#mod-fps60').classList.toggle('on', $('#set-fps60').checked);
  $('#set-resmod').checked = !!settings.TOTK_RENDER_RES;
  if (settings.TOTK_RENDER_RES) $('#set-res').value = settings.TOTK_RENDER_RES;
  showResField();
  $('#set-extra').value = Object.entries(settings).filter(([k]) => !KNOWN.includes(k)).map(([k, v]) => `${k}=${v}`).join('\n');
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
    TOTK_FPS60: $('#set-fps60').checked ? '1' : '',
    TOTK_RENDER_RES: $('#set-resmod').checked ? $('#set-res').value : '',
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
['#set-fps', '#set-fullscreen', '#set-fps60'].forEach((s) => ($(s).onchange = saveSettings));
['#set-gyro', '#set-extra'].forEach((s) => ($(s).onchange = saveSettings));
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
const STEPS = [
  ['check', 'Verificar', 'versão do dump', 1],
  ['romfs', 'RomFS', 'empacotar arquivos', 4],
  ['lift', 'Traduzir', 'código → C', 2],
  ['compile', 'Compilar', 'gerar o .exe', 15],
  ['shaders', 'Shaders', 'para Vulkan', 5],
  ['textures', 'Texturas', 'BC7 / BC1', 7],
  ['finish', 'Concluir', 'configuração', 0.5],
];
const TOTAL = STEPS.reduce((a, s) => a + s[3], 0);
const build = { step: -1, sub: 0, start: 0, stepStart: 0, state: STEPS.map(() => ''), running: false };

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

async function startBuild() {
  build.state = STEPS.map(() => ''); build.step = -1; build.sub = 0; build.start = Date.now();
  $('.bar').classList.remove('error');
  logEl.textContent = '';
  $('#build-phase').textContent = 'Iniciando…';
  renderBuild();
  try {
    await invoke('start_build', { dump: $('#dump-path').dataset.chosen || null, cpus: +$('#cpus').value, only: null });
    setBuilding(true);
  } catch (e) { showError('Não foi possível iniciar a instalação', String(e)); }
}
$('#build').onclick = startBuild;
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
  if (!ok && e.payload === 1) showError('A instalação parou', [...logEl.childNodes].slice(-14).map((n) => n.textContent).join(''));
});

// ---------------------------------------------------------------- play: transition, hand-over, return
async function play() {
  if (!status.installed) { showView('install'); return; }
  const fullscreen = $('#set-fullscreen').checked;
  document.body.classList.add('in-game');
  $('#t-msg').textContent = 'Despertando o reino';
  $('#transition').classList.add('on');
  fadeMusic(0, 1400);
  if (fullscreen) await win.setFullscreen(true);
  await new Promise((r) => setTimeout(r, 1000));   // the transition fades in first
  const pos = await win.outerPosition(), size = await win.outerSize();
  try {
    await invoke('launch_game', {
      rect: [pos.x, pos.y, size.width, size.height], fullscreen, borderless: prefs.frameless,
    });
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

// ---------------------------------------------------------------- start
(async function init() {
  await loadArt();
  buildSlides();
  buildSteps();
  renderBuild();
  const n = await invoke('cpu_count');
  $('#cpus').max = n;
  $('#cpus').value = prefs.cpus && prefs.cpus <= n ? prefs.cpus : n;
  $('#cpus-val').textContent = $('#cpus').value;
  await refresh();
  await loadSettings();
  setBuilding(status.building);
  bgm.volume = 0;
  applyMusic();
  const view = await invoke('start_view');
  if (view && $(`#view-${view}`)) showView(view);
})();

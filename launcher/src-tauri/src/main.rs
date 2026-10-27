// UltraTears launcher: settings (settings.cfg next to UltraTears.exe), the build (tools/setup.py, its output streamed
// to the window) and the game, started hidden over the launcher's own window so the hand-over looks seamless.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::thread;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager, State};

#[cfg(windows)]
use std::os::windows::process::CommandExt;
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

#[derive(Default)]
struct Procs {
    build: Mutex<Option<u32>>,
    game: Mutex<Option<u32>>,
}

#[derive(Serialize)]
struct Status {
    home: String,
    installed: bool,
    tools: bool,
    dump: Option<String>,
    dump_ready: bool,
    version: Option<String>,
    building: bool,
    running: bool,
}

#[derive(Serialize, Clone)]
struct Line {
    text: String,
    err: bool,
}

fn no_window(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW);
    cmd
}

fn config_file(app: &AppHandle) -> Option<PathBuf> {
    app.path().app_config_dir().ok().map(|d| d.join("launcher.json"))
}

fn looks_like_home(p: &Path) -> bool {
    p.join("tools/setup.py").is_file() || p.join("UltraTears.exe").is_file()
}

/// The game folder: ULTRATEARS_HOME, the one chosen in the launcher, or the launcher's own folder.
fn home(app: &AppHandle) -> PathBuf {
    if let Ok(h) = std::env::var("ULTRATEARS_HOME") {
        return PathBuf::from(h);
    }
    if let Some(saved) = config_file(app)
        .and_then(|f| std::fs::read_to_string(f).ok())
        .and_then(|s| serde_json::from_str::<serde_json::Value>(&s).ok())
        .and_then(|v| v.get("home").and_then(|h| h.as_str()).map(PathBuf::from))
    {
        if looks_like_home(&saved) {
            return saved;
        }
    }
    std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)).unwrap_or_default()
}

#[tauri::command]
fn set_home(app: AppHandle, path: String) -> Result<(), String> {
    if !looks_like_home(Path::new(&path)) {
        return Err("Essa pasta não tem o UltraTears (tools\\setup.py ou UltraTears.exe).".into());
    }
    let f = config_file(&app).ok_or("sem pasta de configuração")?;
    std::fs::create_dir_all(f.parent().unwrap()).map_err(|e| e.to_string())?;
    std::fs::write(f, serde_json::json!({ "home": path }).to_string()).map_err(|e| e.to_string())
}

#[tauri::command]
fn status(app: AppHandle, procs: State<Procs>) -> Status {
    let h = home(&app);
    let dump = std::fs::read_to_string(h.join("data/setup.json"))
        .ok()
        .and_then(|s| serde_json::from_str::<serde_json::Value>(&s).ok())
        .and_then(|v| v.get("dump").and_then(|d| d.as_str()).map(String::from));
    let dump_dir = dump.clone().map(PathBuf::from).unwrap_or_else(|| h.join("game"));
    Status {
        installed: h.join("UltraTears.exe").is_file() && h.join("ultratears.cfg").is_file(),
        tools: h.join("deps/python/python.exe").is_file() && h.join("tools/setup.py").is_file(),
        dump_ready: dump_dir.join("exefs/main").is_file() || h.join("data/romfs").is_dir(),
        dump,
        version: std::fs::read_to_string(h.join("VERSION.txt")).ok().and_then(|s| s.lines().next().map(String::from)),
        building: procs.build.lock().unwrap().is_some(),
        running: procs.game.lock().unwrap().is_some(),
        home: h.to_string_lossy().into_owned(),
    }
}

#[derive(Serialize, Default)]
struct Art {
    backgrounds: Vec<String>,
    logo: Option<String>,
    loading: Option<String>,
    music: Option<String>,
}

/// The player's own artwork, from launcher-art\ in the game folder (data: URLs): bg*.jpg/png/webp, logo.png,
/// loading.jpg/png, music.mp3/ogg. The launcher ships with neutral art of its own and uses these instead when present.
#[tauri::command]
fn custom_art(app: AppHandle) -> Art {
    use base64::Engine;
    let dir = home(&app).join("launcher-art");
    let mut art = Art::default();
    let Ok(rd) = std::fs::read_dir(&dir) else { return art };
    let mut names: Vec<String> = rd.filter_map(|e| e.ok()).filter_map(|e| e.file_name().into_string().ok()).collect();
    names.sort();
    let url = |name: &str| -> Option<String> {
        let ext = name.rsplit('.').next()?.to_ascii_lowercase();
        let mime = match ext.as_str() {
            "jpg" | "jpeg" => "image/jpeg",
            "png" => "image/png",
            "webp" => "image/webp",
            "mp3" => "audio/mpeg",
            "ogg" => "audio/ogg",
            _ => return None,
        };
        let bytes = std::fs::read(dir.join(name)).ok()?;
        if bytes.len() > 32 << 20 {
            return None;
        }
        Some(format!("data:{mime};base64,{}", base64::engine::general_purpose::STANDARD.encode(bytes)))
    };
    for n in &names {
        let lower = n.to_ascii_lowercase();
        let stem = lower.rsplit_once('.').map_or(lower.as_str(), |(s, _)| s);
        if stem.starts_with("bg") {
            if let Some(u) = url(n) { art.backgrounds.push(u); }
        } else if stem == "logo" && art.logo.is_none() {
            art.logo = url(n);
        } else if stem == "loading" && art.loading.is_none() {
            art.loading = url(n);
        } else if stem == "music" && art.music.is_none() {
            art.music = url(n);
        }
    }
    art
}

/// `--view <name>` on the command line: the tab to open on (screenshots, shortcuts).
#[tauri::command]
fn start_view() -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    args.iter().position(|a| a == "--view").and_then(|i| args.get(i + 1).cloned())
}

#[tauri::command]
fn cpu_count() -> usize {
    thread::available_parallelism().map(|n| n.get()).unwrap_or(4)
}

/// settings.cfg: "NAME=value" lines the game reads before ultratears.cfg (src/host/boot.c).
#[tauri::command]
fn read_settings(app: AppHandle) -> BTreeMap<String, String> {
    std::fs::read_to_string(home(&app).join("settings.cfg"))
        .unwrap_or_default()
        .lines()
        .filter(|l| !l.starts_with('#'))
        .filter_map(|l| l.split_once('=').map(|(k, v)| (k.trim().to_string(), v.trim().to_string())))
        .collect()
}

#[tauri::command]
fn write_settings(app: AppHandle, settings: BTreeMap<String, String>) -> Result<(), String> {
    let mut text = String::from("# Written by the UltraTears launcher: the player's settings (read before ultratears.cfg).\n");
    for (k, v) in settings.iter().filter(|(_, v)| !v.is_empty()) {
        text += &format!("{k}={v}\n");
    }
    std::fs::write(home(&app).join("settings.cfg"), text).map_err(|e| e.to_string())
}

/// Splits a child's output on \n and \r (progress lines rewrite themselves with \r) and sends each line to the window.
fn stream(app: AppHandle, mut from: impl Read + Send + 'static, err: bool) {
    thread::spawn(move || {
        let mut buf = [0u8; 4096];
        let mut line = Vec::new();
        loop {
            let n = match from.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => n,
            };
            for &b in &buf[..n] {
                if b == b'\n' || b == b'\r' {
                    if !line.is_empty() {
                        let text = String::from_utf8_lossy(&line).into_owned();
                        let _ = app.emit("build-line", Line { text, err });
                        line.clear();
                    }
                } else {
                    line.push(b);
                }
            }
        }
        if !line.is_empty() {
            let _ = app.emit("build-line", Line { text: String::from_utf8_lossy(&line).into_owned(), err });
        }
    });
}

#[tauri::command]
fn start_build(app: AppHandle, procs: State<Procs>, dump: Option<String>, cpus: Option<u32>, only: Option<String>) -> Result<(), String> {
    let mut slot = procs.build.lock().unwrap();
    if slot.is_some() {
        return Err("A instalação já está rodando.".into());
    }
    let h = home(&app);
    let python = h.join("deps/python/python.exe");
    if !python.is_file() {
        return Err(format!("Ferramentas do build não encontradas em {}", h.join("deps").display()));
    }
    let mut cmd = Command::new(python);
    cmd.arg(h.join("tools/setup.py")).current_dir(&h);
    if let Some(d) = dump.filter(|d| !d.is_empty()) {
        cmd.arg("--dump").arg(d);
    }
    if let Some(n) = cpus {
        cmd.arg("--cpus").arg(n.to_string());
    }
    if let Some(o) = only.filter(|o| !o.is_empty()) {
        cmd.arg("--only").arg(o);
    }
    cmd.env("PYTHONUNBUFFERED", "1").env("PYTHONNOUSERSITE", "1").env("PYTHONIOENCODING", "utf-8");
    cmd.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child: Child = no_window(&mut cmd).spawn().map_err(|e| e.to_string())?;
    stream(app.clone(), child.stdout.take().unwrap(), false);
    stream(app.clone(), child.stderr.take().unwrap(), true);
    *slot = Some(child.id());
    drop(slot);
    thread::spawn(move || {
        let code = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
        *app.state::<Procs>().build.lock().unwrap() = None;
        let _ = app.emit("build-done", code);
    });
    Ok(())
}

/// The heap overlay the resolution mod needs for `size` (tools/make_res_overlay.py, about a second).
#[tauri::command]
fn make_res_overlay(app: AppHandle, size: String) -> Result<(), String> {
    if !size.split_once('x').map_or(false, |(w, h)| w.parse::<u32>().is_ok() && h.parse::<u32>().is_ok()) {
        return Err(format!("resolução inválida: {size}"));
    }
    let h = home(&app);
    let python = h.join("deps/python/python.exe");
    let script = h.join("tools/make_res_overlay.py");
    if !python.is_file() || !script.is_file() {
        return Err("tools\\make_res_overlay.py ou o Python do build não estão na pasta do jogo.".into());
    }
    let mut cmd = Command::new(python);
    cmd.arg(script).arg(&size).current_dir(&h).env("PYTHONNOUSERSITE", "1");
    let out = no_window(&mut cmd).output().map_err(|e| e.to_string())?;
    if out.status.success() {
        Ok(())
    } else {
        Err(String::from_utf8_lossy(&out.stderr).trim().to_string() + &String::from_utf8_lossy(&out.stdout))
    }
}

fn kill_tree(pid: u32) {
    let _ = no_window(Command::new("taskkill").args(["/PID", &pid.to_string(), "/T", "/F"])).status();
}

#[tauri::command]
fn cancel_build(procs: State<Procs>) {
    if let Some(pid) = *procs.build.lock().unwrap() {
        kill_tree(pid);
    }
}

#[cfg(windows)]
fn has_visible_window(pid: u32) -> bool {
    use windows_sys::Win32::Foundation::{BOOL, HWND, LPARAM};
    use windows_sys::Win32::UI::WindowsAndMessaging::{EnumWindows, GetWindowThreadProcessId, IsWindowVisible};
    unsafe extern "system" fn each(hwnd: HWND, lp: LPARAM) -> BOOL {
        let found = &mut *(lp as *mut (u32, bool));
        let mut owner = 0u32;
        GetWindowThreadProcessId(hwnd, &mut owner);
        if owner == found.0 && IsWindowVisible(hwnd) != 0 {
            found.1 = true;
            return 0;
        }
        1
    }
    let mut found = (pid, false);
    unsafe { EnumWindows(Some(each), &mut found as *mut _ as LPARAM) };
    found.1
}

#[cfg(not(windows))]
fn has_visible_window(_pid: u32) -> bool {
    true
}

/// Starts the game hidden over the rectangle (x, y, w, h in pixels); "game-visible" when its window shows its first
/// frame, "game-exit" with the exit code when it ends.
#[tauri::command]
fn launch_game(app: AppHandle, procs: State<Procs>, rect: (i32, i32, i32, i32), fullscreen: bool, borderless: bool) -> Result<(), String> {
    let mut slot = procs.game.lock().unwrap();
    if slot.is_some() {
        return Err("O jogo já está aberto.".into());
    }
    let h = home(&app);
    let exe = h.join("UltraTears.exe");
    if !exe.is_file() {
        return Err("UltraTears.exe não existe ainda: faça a instalação primeiro.".into());
    }
    let mut cmd = Command::new(exe);
    cmd.current_dir(&h)
        .env("TOTK_START_HIDDEN", "1")
        .env("TOTK_WINDOW", format!("{},{},{},{}", rect.0, rect.1, rect.2, rect.3));
    if fullscreen {
        cmd.env("TOTK_FULLSCREEN", "1");
    }
    if borderless {
        cmd.env("TOTK_BORDERLESS", "1");
    }
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let pid = child.id();
    *slot = Some(pid);
    drop(slot);
    let watcher = app.clone();
    thread::spawn(move || {
        for _ in 0..(10 * 60 * 10) {   // up to 10 minutes for the first frame
            if has_visible_window(pid) {
                let _ = watcher.emit("game-visible", ());
                return;
            }
            if watcher.state::<Procs>().game.lock().unwrap().is_none() {
                return;
            }
            thread::sleep(Duration::from_millis(100));
        }
    });
    thread::spawn(move || {
        let code = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
        *app.state::<Procs>().game.lock().unwrap() = None;
        let _ = app.emit("game-exit", code);
    });
    Ok(())
}

#[tauri::command]
fn stop_game(procs: State<Procs>) {
    if let Some(pid) = *procs.game.lock().unwrap() {
        kill_tree(pid);
    }
}

/// The end of the game's log (lasrecomp.log next to the exe), for the error shown when it stops.
#[tauri::command]
fn log_tail(app: AppHandle, lines: usize) -> String {
    let text = std::fs::read(home(&app).join("lasrecomp.log")).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
    let all: Vec<&str> = text.lines().collect();
    all[all.len().saturating_sub(lines)..].join("\n")
}

#[tauri::command]
fn open_path(app: AppHandle, path: String) -> Result<(), String> {
    if path.starts_with("https://") {   // the default browser
        return Command::new("explorer").arg(&path).spawn().map(|_| ()).map_err(|e| e.to_string());
    }
    let p = if Path::new(&path).is_absolute() { PathBuf::from(&path) } else { home(&app).join(&path) };
    if path == "launcher-art" {
        let _ = std::fs::create_dir_all(&p);
    }
    Command::new("explorer").arg(p).spawn().map(|_| ()).map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(Procs::default())
        .invoke_handler(tauri::generate_handler![
            status, set_home, start_view, cpu_count, make_res_overlay, custom_art, read_settings, write_settings, start_build, cancel_build, launch_game, stop_game,
            log_tail, open_path
        ])
        .run(tauri::generate_context!())
        .expect("UltraTears launcher");
}

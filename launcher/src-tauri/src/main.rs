// UltraTears launcher: settings (settings.cfg next to UltraTears.exe), the build (tools/setup.py, its output streamed
// to the window) and the game, started hidden over the launcher's own window so the hand-over looks seamless.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::thread;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager, State};

mod game_update;

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
    packed: bool,         // the RomFS is in data/romfs: the dump is no longer needed
    temp_dump: bool,      // game/ was made by an extension (remove_temp_dump may delete it)
    can_build: bool,      // setup.py in the game folder, or a source checkout to fill it from
    free_gb: Option<f64>, // free space where the game folder is
    version: Option<String>,
    building: bool,
    running: bool,
    dev: bool,
    commit: Option<String>,
}

#[derive(Serialize, Clone)]
struct Line {
    text: String,
    err: bool,
}

#[cfg(unix)]
use std::os::unix::process::CommandExt;

const GAME_EXE: &str = if cfg!(windows) { "UltraTears.exe" } else { "UltraTears" };

/// No console window (Windows); its own process group, so kill_tree ends what it started too (elsewhere).
fn no_window(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW);
    #[cfg(unix)]
    cmd.process_group(0);
    cmd
}

/// The build's Python: the builder package's own, else (macOS, Linux) the system's.
fn python(h: &Path) -> Option<PathBuf> {
    let own = h.join(if cfg!(windows) { "deps/python/python.exe" } else { "deps/python/bin/python3" });
    let system: &[&str] = if cfg!(windows) { &[] } else { &["/opt/homebrew/bin/python3", "/usr/local/bin/python3", "/usr/bin/python3"] };
    std::iter::once(own).chain(system.iter().map(PathBuf::from)).find(|p| p.is_file())
}

fn config_file(app: &AppHandle) -> Option<PathBuf> {
    app.path().app_config_dir().ok().map(|d| d.join("launcher.json"))
}

fn looks_like_home(p: &Path) -> bool {
    p.join("tools/setup.py").is_file() || p.join(GAME_EXE).is_file()
}

/// Free bytes on the disk of `p` (or of its nearest existing parent).
fn free_space(p: &Path) -> Option<u64> {
    let p = p.ancestors().find(|a| a.exists())?;
    #[cfg(unix)]
    {
        let c = std::ffi::CString::new(p.as_os_str().as_encoded_bytes()).ok()?;
        let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
        if unsafe { libc::statvfs(c.as_ptr(), &mut st) } != 0 {
            return None;
        }
        Some(st.f_bavail as u64 * st.f_frsize as u64)
    }
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        let w: Vec<u16> = p.as_os_str().encode_wide().chain(Some(0)).collect();
        let mut free = 0u64;
        let ok = unsafe { windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW(w.as_ptr(), &mut free, std::ptr::null_mut(), std::ptr::null_mut()) };
        (ok != 0).then_some(free)
    }
}

/// The source checkout the launcher can fill an empty game folder from (setup.py --output): ULTRATEARS_SRC, or the
/// one the launcher itself was built in (launcher/src-tauri/target/<profile>/). Only a Git checkout counts: a game folder
/// holding the launcher has the downloaded source too, and setup.py refuses --output there.
fn source_checkout() -> Option<PathBuf> {
    let env = std::env::var("ULTRATEARS_SRC").ok().map(PathBuf::from);
    let exe = std::env::current_exe().ok();
    env.into_iter()
        .chain(exe.iter().flat_map(|e| e.ancestors().skip(1).take(8).map(Path::to_path_buf)))
        .find(|d| d.join("tools/setup.py").is_file() && d.join("src/runtime").is_dir() && d.join(".git").exists())
}

const TEMP_MARK: &str = ".made-by-extension";

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
        if saved.is_dir() {
            return saved;
        }
    }
    std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)).unwrap_or_default()
}

#[tauri::command]
fn set_home(app: AppHandle, path: String) -> Result<(), String> {
    let p = Path::new(&path);
    if !looks_like_home(p) && source_checkout().is_none() {
        return Err("Essa pasta não tem o UltraTears (tools/setup.py ou o jogo).".into());
    }
    std::fs::create_dir_all(p).map_err(|e| e.to_string())?;   // an empty folder: the build fills it
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
        installed: h.join(GAME_EXE).is_file() && h.join("ultratears.cfg").is_file(),
        tools: python(&h).is_some() && h.join("tools/setup.py").is_file(),
        dump_ready: dump_dir.join("exefs/main").is_file() || h.join("data/romfs").is_dir(),
        packed: h.join("data/romfs/romfs.bin").is_file() && h.join("data/exefs/main").is_file(),
        temp_dump: h.join("game").join(TEMP_MARK).is_file(),
        can_build: true,   // (without the source here, game_update.rs downloads it first)
        dev: source_checkout().is_some(),
        commit: game_update::installed_commit(&h),
        free_gb: free_space(&h).map(|b| b as f64 / 1e9),
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
fn stream(app: AppHandle, mut from: impl Read + Send + 'static, err: bool, event: &'static str) {
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
                        let _ = app.emit(event, Line { text, err });
                        line.clear();
                    }
                } else {
                    line.push(b);
                }
            }
        }
        if !line.is_empty() {
            let _ = app.emit(event, Line { text: String::from_utf8_lossy(&line).into_owned(), err });
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
    // A source checkout next to the launcher (development) fills the game folder (setup.py --output) and builds there.
    // Otherwise the folder has its own source (game_update.rs downloads it; or a builder package): setup.py in place.
    let checkout = source_checkout();
    let own = checkout.is_none();
    let src = match checkout {
        Some(c) => c,
        None if h.join("tools/setup.py").is_file() => h.clone(),
        None => return Err("O código do jogo ainda não foi baixado para esta pasta.".into()),
    };
    let Some(python) = python(&src).or_else(|| python(&h)) else {   // (a checkout has no deps\: the game folder's)
        return Err(format!("Ferramentas do build não encontradas em {}", src.join("deps").display()));
    };
    let mut cmd = Command::new(python);
    cmd.arg(src.join("tools/setup.py")).current_dir(&src);
    if !own {
        cmd.arg("--output").arg(&h);
    }
    if let Some(d) = dump.filter(|d| !d.is_empty()) {
        cmd.arg("--dump").arg(d);
    }
    if let Some(n) = cpus {
        cmd.arg("--cpus").arg(n.to_string());
    }
    if let Some(o) = only.filter(|o| !o.is_empty()) {
        cmd.arg("--only").arg(o);
    }
    // ULTRATEARS_NO_PROMPT: nobody answers questions here (setup.py's offer to delete the dump: the launcher has its own)
    cmd.env("PYTHONUNBUFFERED", "1").env("PYTHONNOUSERSITE", "1").env("PYTHONIOENCODING", "utf-8").env("ULTRATEARS_NO_PROMPT", "1");
    cmd.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child: Child = no_window(&mut cmd).spawn().map_err(|e| e.to_string())?;
    stream(app.clone(), child.stdout.take().unwrap(), false, "build-line");
    stream(app.clone(), child.stderr.take().unwrap(), true, "build-line");
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
    let script = h.join("tools/make_res_overlay.py");
    let Some(python) = python(&h).filter(|_| script.is_file()) else {
        return Err("tools/make_res_overlay.py ou o Python do build não estão na pasta do jogo.".into());
    };
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
    #[cfg(windows)]
    let _ = no_window(Command::new("taskkill").args(["/PID", &pid.to_string(), "/T", "/F"])).status();
    #[cfg(unix)]   // the whole group (no_window gave each child its own)
    let _ = Command::new("kill").args(["-KILL", &format!("-{pid}")]).status();
}

#[tauri::command]
fn cancel_build(procs: State<Procs>) {
    if let Some(pid) = *procs.build.lock().unwrap() {
        kill_tree(pid);
    }
}

#[cfg(windows)]
fn has_visible_window(pid: u32, _home: &Path) -> bool {
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

/// The game's start-up status (startup.txt in the game folder, written by host/window_sdl.c while it starts hidden):
/// "starting", "shaders <done> <total>", "ready"; empty before the game writes it.
#[tauri::command]
fn startup_status(app: AppHandle) -> String {
    std::fs::read_to_string(home(&app).join("startup.txt")).unwrap_or_default().trim().to_string()
}

/// macOS / Linux: no window enumeration here; the game says "ready" in startup.txt when its window shows.
#[cfg(not(windows))]
fn has_visible_window(_pid: u32, home: &Path) -> bool {
    std::fs::read_to_string(home.join("startup.txt")).map_or(false, |s| s.starts_with("ready"))
}

/// Starts the game hidden over the rectangle (x, y, w, h in pixels); "game-visible" when its window shows its first
/// frame, "game-exit" with the exit code when it ends.
#[tauri::command]
fn launch_game(app: AppHandle, procs: State<Procs>, rect: (i32, i32, i32, i32), fullscreen: bool, borderless: bool, lang: Option<String>) -> Result<(), String> {
    let mut slot = procs.game.lock().unwrap();
    if slot.is_some() {
        return Err("O jogo já está aberto.".into());
    }
    let h = home(&app);
    let exe = h.join(GAME_EXE);
    if !exe.is_file() {
        return Err(format!("{GAME_EXE} não existe ainda: faça a instalação primeiro."));
    }
    let _ = std::fs::remove_file(h.join("startup.txt"));   // (a previous run's)
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
    if let Some(l) = lang.filter(|l| l == "en" || l == "pt") {
        cmd.env("TOTK_LANG", l);   // the game's own notices (overlay) in the launcher's language
    }
    #[cfg(unix)]
    cmd.process_group(0);
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let pid = child.id();
    *slot = Some(pid);
    drop(slot);
    let watcher = app.clone();
    let game_home = h.clone();
    thread::spawn(move || {
        for _ in 0..(10 * 60 * 10) {   // up to 10 minutes for the first frame
            if has_visible_window(pid, &game_home) {
                #[cfg(target_os = "macos")]   // one Dock icon: the game's now (it enters the Dock with its window)
                let _ = watcher.set_dock_visibility(false);
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
        #[cfg(target_os = "macos")]
        let _ = app.set_dock_visibility(true);
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

const OPENER: &str = if cfg!(windows) { "explorer" } else if cfg!(target_os = "macos") { "open" } else { "xdg-open" };

#[tauri::command]
fn open_path(app: AppHandle, path: String) -> Result<(), String> {
    if path.starts_with("https://") {   // the default browser
        return Command::new(OPENER).arg(&path).spawn().map(|_| ()).map_err(|e| e.to_string());
    }
    let p = if Path::new(&path).is_absolute() { PathBuf::from(&path) } else { home(&app).join(&path) };
    if path == "launcher-art" {
        let _ = std::fs::create_dir_all(&p);
    }
    Command::new(OPENER).arg(p).spawn().map(|_| ()).map_err(|e| e.to_string())
}

// ---- extensions: <app data>/extensions/<id>/extension.json + its program, added from a .zip in the launcher.
// kind "dump-source": a program that writes exefs/ and romfs/ into {out} from the player's inputs (files it asks
// for); its stdout lines "progress <done> <total>" drive the bar, the exit code says how it went.

#[derive(Serialize, Deserialize, Clone)]
struct ExtInput {
    key: String,
    label: String,
    #[serde(rename = "type")]
    kind: String,   // "file" | "files" | "dir"
    #[serde(default)]
    extensions: Vec<String>,
}

#[derive(Serialize, Deserialize, Clone)]
struct Extension {
    id: String,
    name: String,
    version: String,
    kind: String,
    #[serde(default)]
    description: String,
    #[serde(default)]
    bin: BTreeMap<String, String>,   // "<os>-<arch>" -> program, relative to its folder
    #[serde(default)]
    inputs: Vec<ExtInput>,
    #[serde(default)]
    args: Vec<String>,   // "{key}" alone: every value of that input; "{out}": the output folder
    #[serde(default, skip_deserializing)]
    supported: bool,   // has a program for this OS and CPU
}

fn platform() -> String {
    format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH)
}

fn extensions_dir(app: &AppHandle) -> Result<PathBuf, String> {
    app.path().app_data_dir().map(|d| d.join("extensions")).map_err(|e| e.to_string())
}

fn read_extension(dir: &Path) -> Option<Extension> {
    let mut e: Extension = serde_json::from_str(&std::fs::read_to_string(dir.join("extension.json")).ok()?).ok()?;
    e.supported = e.bin.get(&platform()).map_or(false, |b| dir.join(b).is_file());
    Some(e)
}

fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 64 && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_')
}

#[tauri::command]
fn list_extensions(app: AppHandle) -> Vec<Extension> {
    let Ok(dir) = extensions_dir(&app) else { return Vec::new() };
    let mut all: Vec<Extension> = std::fs::read_dir(dir)
        .map(|rd| rd.filter_map(|e| e.ok()).filter_map(|e| read_extension(&e.path())).collect())
        .unwrap_or_default();
    all.sort_by(|a, b| a.name.cmp(&b.name));
    all
}

/// Unpacks the .zip (extension.json at its root) into extensions/<id>, replacing an older version of the same id.
#[tauri::command]
fn install_extension(app: AppHandle, zip: String) -> Result<Extension, String> {
    let file = std::fs::File::open(&zip).map_err(|e| e.to_string())?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| format!("não é um .zip válido: {e}"))?;
    let manifest: Extension = {
        let mut f = archive.by_name("extension.json").map_err(|_| "o .zip não tem extension.json na raiz")?;
        let mut text = String::new();
        f.read_to_string(&mut text).map_err(|e| e.to_string())?;
        serde_json::from_str(&text).map_err(|e| format!("extension.json inválido: {e}"))?
    };
    if !valid_id(&manifest.id) {
        return Err(format!("id de extensão inválido: {}", manifest.id));
    }
    let root = extensions_dir(&app)?;
    let tmp = root.join(format!(".new-{}", manifest.id));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
    archive.extract(&tmp).map_err(|e| e.to_string())?;   // (names outside the folder are refused)
    let dst = root.join(&manifest.id);
    let _ = std::fs::remove_dir_all(&dst);
    std::fs::rename(&tmp, &dst).map_err(|e| e.to_string())?;
    let ext = read_extension(&dst).ok_or("extension.json inválido")?;
    if !ext.supported {
        return Err(format!("{} foi adicionada, mas não tem programa para este sistema ({})", ext.name, platform()));
    }
    Ok(ext)
}

#[tauri::command]
fn remove_extension(app: AppHandle, id: String) -> Result<(), String> {
    if !valid_id(&id) {
        return Err("id inválido".into());
    }
    std::fs::remove_dir_all(extensions_dir(&app)?.join(id)).map_err(|e| e.to_string())
}

/// Runs the extension's program with the player's inputs ("ext-line" per output line, "ext-done" with the exit
/// code). It takes the build's slot: one or the other at a time.
#[tauri::command]
fn run_extension(app: AppHandle, procs: State<Procs>, id: String, inputs: BTreeMap<String, Vec<String>>, out: String) -> Result<(), String> {
    let mut slot = procs.build.lock().unwrap();
    if slot.is_some() {
        return Err("Uma instalação ou extração já está rodando.".into());
    }
    if !valid_id(&id) {
        return Err("id inválido".into());
    }
    let dir = extensions_dir(&app)?.join(&id);
    let ext = read_extension(&dir).ok_or("extensão não encontrada")?;
    let program = ext.bin.get(&platform()).map(|b| dir.join(b)).filter(|p| p.is_file())
        .ok_or_else(|| format!("{} não tem programa para este sistema ({})", ext.name, platform()))?;
    for i in &ext.inputs {
        if inputs.get(&i.key).map_or(true, |v| v.is_empty()) {
            return Err(format!("Escolha: {}", i.label));
        }
    }
    let out = if out.is_empty() { home(&app).join("game") } else { PathBuf::from(out) };
    let mark = out.join(TEMP_MARK);
    std::fs::create_dir_all(&out).map_err(|e| e.to_string())?;
    let mut cmd = Command::new(program);
    for a in &ext.args {
        if a == "{out}" {
            cmd.arg(&out);
        } else if let Some(v) = a.strip_prefix('{').and_then(|k| k.strip_suffix('}')).and_then(|k| inputs.get(k)) {
            cmd.args(v);
        } else {
            cmd.arg(a);
        }
    }
    cmd.current_dir(&dir).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child: Child = no_window(&mut cmd).spawn().map_err(|e| e.to_string())?;
    stream(app.clone(), child.stdout.take().unwrap(), false, "ext-line");
    stream(app.clone(), child.stderr.take().unwrap(), true, "ext-line");
    *slot = Some(child.id());
    drop(slot);
    thread::spawn(move || {
        let code = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
        if code == 0 {   // a dump only the launcher made: it may delete it once the build has taken it in
            let _ = std::fs::write(&mark, "");
        }
        *app.state::<Procs>().build.lock().unwrap() = None;
        let _ = app.emit("ext-done", code);
    });
    Ok(())
}

/// Deletes game/ in the game folder once the build has packed it (data/romfs, data/exefs), and only when an
/// extension made it (TEMP_MARK): a dump the player put there is never touched. Returns the GB freed.
#[tauri::command]
fn remove_temp_dump(app: AppHandle) -> Result<f64, String> {
    let h = home(&app);
    let dump = h.join("game");
    if !dump.join(TEMP_MARK).is_file() {
        return Ok(0.0);
    }
    if !(h.join("data/romfs/romfs.bin").is_file() && h.join("data/romfs/runtime-index.bin").is_file() && h.join("data/exefs/main").is_file()) {
        return Err("o build ainda não empacotou o RomFS".into());
    }
    let before = free_space(&h).unwrap_or(0);
    std::fs::remove_dir_all(&dump).map_err(|e| e.to_string())?;
    Ok(free_space(&h).unwrap_or(0).saturating_sub(before) as f64 / 1e9)
}

/// Game controllers drive the launcher too: "pad" events with "up" / "down" / "left" / "right" (d-pad or left stick,
/// repeating while held), "ok" (the bottom face button), "back" (the right one) and "start". Nothing while the game
/// runs (it reads the controller then).
fn pad_thread(app: AppHandle) {
    use gilrs::{Axis, Button, EventType};
    thread::spawn(move || {
        let Ok(mut gil) = gilrs::Gilrs::new() else { return };
        let mut held: Option<(&'static str, std::time::Instant)> = None;   // the direction held and when it repeats
        loop {
            let mut out: Vec<&'static str> = Vec::new();
            while let Some(gilrs::Event { event, .. }) = gil.next_event() {
                if let EventType::ButtonPressed(b, _) = event {
                    match b {
                        Button::South => out.push("ok"),
                        Button::East => out.push("back"),
                        Button::Start => out.push("start"),
                        _ => {}
                    }
                }
            }
            let mut dir = None;
            for (_, g) in gil.gamepads() {
                let (x, y) = (g.value(Axis::LeftStickX), g.value(Axis::LeftStickY));
                dir = dir
                    .or(g.is_pressed(Button::DPadUp).then_some("up"))
                    .or(g.is_pressed(Button::DPadDown).then_some("down"))
                    .or(g.is_pressed(Button::DPadLeft).then_some("left"))
                    .or(g.is_pressed(Button::DPadRight).then_some("right"))
                    .or((y > 0.6).then_some("up"))
                    .or((y < -0.6).then_some("down"))
                    .or((x < -0.6).then_some("left"))
                    .or((x > 0.6).then_some("right"));
            }
            let now = std::time::Instant::now();
            held = match (dir, held) {
                (Some(d), Some((h, at))) if d == h => {
                    if now < at { Some((h, at)) } else { out.push(d); Some((d, now + Duration::from_millis(110))) }
                }
                (Some(d), _) => { out.push(d); Some((d, now + Duration::from_millis(380))) }
                (None, _) => None,
            };
            if !out.is_empty() && app.state::<Procs>().game.lock().unwrap().is_none() {
                for e in out {
                    let _ = app.emit("pad", e);
                }
            }
            thread::sleep(Duration::from_millis(16));
        }
    });
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .plugin(tauri_plugin_process::init())
        .manage(Procs::default())
        .setup(|app| {
            pad_thread(app.handle().clone());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            status, set_home, start_view, cpu_count, make_res_overlay, custom_art, read_settings, write_settings, start_build, cancel_build, launch_game, stop_game,
            log_tail, open_path, list_extensions, install_extension, remove_extension, run_extension, remove_temp_dump, startup_status,
            game_update::game_update_check, game_update::game_update_apply
        ])
        .run(tauri::generate_context!())
        .expect("UltraTears launcher");
}

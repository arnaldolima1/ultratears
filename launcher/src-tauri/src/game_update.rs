//! Game updates (fork): the game's source comes from GitHub (GAME_REPO, branch GAME_BRANCH) as a zip of one commit,
//! unpacked over the game folder; the build then runs there in place, rebuilding only what changed. The commit the
//! folder holds is in its VERSION.txt. With a source checkout next to the launcher (development) the checkout is the
//! source instead and nothing is downloaded.
use crate::{home, python, source_checkout, Procs};
use serde::Serialize;
use serde_json::Value;
use std::collections::BTreeSet;
use std::io::{Read, Write};
use std::path::Path;
use std::thread;
use tauri::{AppHandle, Emitter, Manager, State};

pub const GAME_REPO: &str = "arnaldolima1/ultratears";
pub const GAME_BRANCH: &str = "main";
/// Top-level folders of the game folder that are the player's or the build's, never source: left alone.
const KEEP: &[&str] = &["game", "data", "build", "userdata", "diag", "deps", "dist", "dev-local", "launcher-art", "extensions", ".github"];
/// The Windows build tools (Python, clang, ninja, libraries) come from the upstream project's builder package.
const BUILDER_RELEASES: &str = "https://api.github.com/repos/kuraeow/ultratears/releases?per_page=20";

fn get(url: &str) -> Result<ureq::Response, String> {
    ureq::get(url)
        .set("User-Agent", "UltraTears-launcher")
        .set("Accept", "application/vnd.github+json")
        .call()
        .map_err(|e| match e {
            ureq::Error::Status(403, _) => "O GitHub limitou as consultas deste IP por agora; tente de novo em alguns minutos.".to_string(),
            e => format!("Sem conexão com o GitHub: {e}"),
        })
}

fn get_json(url: &str) -> Result<Value, String> {
    get(url)?.into_json::<Value>().map_err(|e| e.to_string())
}

/// The commit in the folder's VERSION.txt ("UltraTears <version> (commit <hash>)").
pub fn installed_commit(h: &Path) -> Option<String> {
    let text = std::fs::read_to_string(h.join("VERSION.txt")).ok()?;
    let c = text.split("(commit ").nth(1)?.split(')').next()?.trim().to_string();
    (!c.is_empty() && c != "unknown").then_some(c)
}

#[derive(Serialize)]
pub struct GameUpdate {
    dev: bool,
    available: bool,
    local: Option<String>,
    remote: String,
    date: String,
    ahead: Option<u64>,
    commits: Vec<String>,
}

/// Whether GAME_BRANCH has commits the game folder does not (and their titles, newest first).
#[tauri::command]
pub fn game_update_check(app: AppHandle) -> Result<GameUpdate, String> {
    let h = home(&app);
    let local = installed_commit(&h);
    if source_checkout().is_some() {
        return Ok(GameUpdate { dev: true, available: false, local, remote: String::new(), date: String::new(), ahead: None, commits: vec![] });
    }
    let head = get_json(&format!("https://api.github.com/repos/{GAME_REPO}/commits/{GAME_BRANCH}"))?;
    let remote = head["sha"].as_str().unwrap_or_default().to_string();
    let date = head["commit"]["committer"]["date"].as_str().unwrap_or_default().to_string();
    let title = |c: &Value| c["commit"]["message"].as_str().unwrap_or_default().lines().next().unwrap_or_default().to_string();
    let current = local.as_deref().is_some_and(|l| remote.starts_with(l));
    let mut ahead = None;
    let mut commits = vec![];
    if let Some(l) = local.as_deref().filter(|_| !current) {
        if let Ok(cmp) = get_json(&format!("https://api.github.com/repos/{GAME_REPO}/compare/{l}...{remote}")) {
            ahead = cmp["ahead_by"].as_u64();
            if let Some(list) = cmp["commits"].as_array() {
                commits = list.iter().rev().take(15).map(title).collect();
            }
        }
    }
    if commits.is_empty() && !current {
        commits.push(title(&head));
    }
    Ok(GameUpdate { dev: false, available: !current, local, remote, date, ahead, commits })
}

#[derive(Serialize, Clone)]
struct Progress {
    text: String,
    done: u64,
    total: u64,
}

fn say(app: &AppHandle, text: &str, done: u64, total: u64) {
    let _ = app.emit("update-progress", Progress { text: text.into(), done, total });
}

/// Downloads url to path (through path.part), reporting progress as `what`.
fn download(app: &AppHandle, url: &str, path: &Path, what: &str) -> Result<(), String> {
    let resp = get(url)?;
    let total = resp.header("Content-Length").and_then(|v| v.parse().ok()).unwrap_or(0);
    let part = path.with_extension("part");
    let mut out = std::fs::File::create(&part).map_err(|e| e.to_string())?;
    let mut reader = resp.into_reader();
    let (mut buf, mut done, mut shown) = (vec![0u8; 1 << 16], 0u64, 0u64);
    loop {
        let n = reader.read(&mut buf).map_err(|e| format!("Download interrompido: {e}"))?;
        if n == 0 {
            break;
        }
        out.write_all(&buf[..n]).map_err(|e| e.to_string())?;
        done += n as u64;
        if done - shown > 1 << 20 {
            shown = done;
            say(app, what, done, total);
        }
    }
    drop(out);
    std::fs::rename(&part, path).map_err(|e| e.to_string())
}

/// Unpacks the files of `zip` whose path, after dropping its first `skip` parts, passes `keep`, into `dest`. Returns
/// their paths (relative, '/').
fn unpack(zip: &Path, dest: &Path, keep: impl Fn(&[&str]) -> Option<usize>) -> Result<Vec<String>, String> {
    let file = std::fs::File::open(zip).map_err(|e| e.to_string())?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| format!("Arquivo baixado inválido: {e}"))?;
    let mut written = vec![];
    for i in 0..archive.len() {
        let mut entry = archive.by_index(i).map_err(|e| e.to_string())?;
        if entry.is_dir() {
            continue;
        }
        let Some(name) = entry.enclosed_name() else { continue };
        let parts: Vec<String> = name.components().map(|c| c.as_os_str().to_string_lossy().into_owned()).collect();
        let refs: Vec<&str> = parts.iter().map(String::as_str).collect();
        let Some(from) = keep(&refs) else { continue };
        let rel = parts[from..].join("/");
        let target = dest.join(&rel);
        if let Some(p) = target.parent() {
            std::fs::create_dir_all(p).map_err(|e| e.to_string())?;
        }
        let mut out = std::fs::File::create(&target).map_err(|e| format!("{}: {e}", target.display()))?;
        std::io::copy(&mut entry, &mut out).map_err(|e| e.to_string())?;
        #[cfg(unix)]
        if let Some(mode) = entry.unix_mode() {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&target, std::fs::Permissions::from_mode(mode & 0o755));
        }
        written.push(rel);
    }
    Ok(written)
}

/// Windows: the build tools from the upstream builder package (deps\ and licenses\), when the folder has none.
fn ensure_builder(app: &AppHandle, h: &Path, cache: &Path) -> Result<(), String> {
    if !cfg!(windows) || python(h).is_some() {
        return Ok(());
    }
    say(app, "Procurando as ferramentas do build…", 0, 0);
    let releases = get_json(BUILDER_RELEASES)?;
    let asset = releases.as_array().into_iter().flatten()
        .flat_map(|r| r["assets"].as_array().cloned().unwrap_or_default())
        .find(|a| a["name"].as_str().is_some_and(|n| n.starts_with("UltraTears-builder-") && n.ends_with("-windows-x86_64.zip")))
        .ok_or("Nenhum pacote de ferramentas do build foi encontrado nos releases do UltraTears.")?;
    let name = asset["name"].as_str().unwrap_or("builder.zip");
    let zip = cache.join(name);
    if !zip.is_file() {
        download(app, asset["browser_download_url"].as_str().unwrap_or_default(), &zip, "Baixando as ferramentas do build")?;
    }
    say(app, "Instalando as ferramentas do build…", 0, 0);
    unpack(&zip, h, |p| p.iter().position(|c| *c == "deps" || *c == "licenses"))?;
    if python(h).is_none() {
        return Err("O pacote de ferramentas não trouxe o Python do build.".into());
    }
    Ok(())
}

/// Unpacks the source archive of commit `short` over the game folder `h`: the files of its first folder
/// ("<repo>-<sha>/") except the player's and the build's folders; source files the previous version listed in
/// setup-files.txt and this one no longer has are removed; setup-files.txt and VERSION.txt describe the new one.
fn install_source(zip: &Path, h: &Path, short: &str) -> Result<usize, String> {
    let files = unpack(zip, h, |p| (p.len() > 1 && !KEEP.contains(&p[1])).then_some(1))?;
    let new: BTreeSet<&str> = files.iter().map(String::as_str).collect();
    let listing = h.join("setup-files.txt");
    for old in std::fs::read_to_string(&listing).unwrap_or_default().lines() {
        let first = old.split('/').next().unwrap_or_default();
        if !old.is_empty() && !new.contains(old) && !KEEP.contains(&first) && !old.contains("..") {
            let _ = std::fs::remove_file(h.join(old));   // a source file the new version no longer has
        }
    }
    std::fs::write(&listing, files.join("\n") + "\n").map_err(|e| e.to_string())?;
    std::fs::write(h.join("VERSION.txt"), format!("UltraTears {GAME_BRANCH}-{short} (commit {short})\n")).map_err(|e| e.to_string())?;
    Ok(files.len())
}

fn apply(app: &AppHandle, sha: &str) -> Result<(), String> {
    let h = home(app);
    std::fs::create_dir_all(&h).map_err(|e| e.to_string())?;
    let cache = app.path().app_cache_dir().map_err(|e| e.to_string())?.join("downloads");
    std::fs::create_dir_all(&cache).map_err(|e| e.to_string())?;
    let short = &sha[..sha.len().min(7)];
    let zip = cache.join(format!("ultratears-{short}.zip"));
    if !zip.is_file() {
        download(app, &format!("https://codeload.github.com/{GAME_REPO}/zip/{sha}"), &zip, "Baixando o código do jogo")?;
    }
    say(app, "Atualizando os arquivos…", 0, 0);
    install_source(&zip, &h, short)?;
    ensure_builder(app, &h, &cache)?;
    // the older downloads of the source are not needed again
    if let Ok(rd) = std::fs::read_dir(&cache) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n.starts_with("ultratears-") && n.ends_with(".zip") && e.path() != zip {
                let _ = std::fs::remove_file(e.path());
            }
        }
    }
    Ok(())
}

/// Brings the game folder's source to commit `sha` of GAME_BRANCH ("update-progress" events, then "update-done" with
/// null or the error). The build that follows is start_build's, in place.
#[tauri::command]
pub fn game_update_apply(app: AppHandle, procs: State<Procs>, sha: String) -> Result<(), String> {
    if procs.build.lock().unwrap().is_some() {
        return Err("Uma instalação ou extração já está rodando.".into());
    }
    if sha.len() < 7 || !sha.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err(format!("commit inválido: {sha}"));
    }
    thread::spawn(move || {
        let result = apply(&app, &sha);
        let _ = app.emit("update-done", result.err());
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    // ULTRATEARS_TEST_ZIP=<a source archive from codeload> cargo test: unpacks it into a temporary folder
    #[test]
    fn install_source_keeps_the_players_folders() {
        let Ok(zip) = std::env::var("ULTRATEARS_TEST_ZIP") else { return };
        let h = std::env::temp_dir().join(format!("ut-src-test-{}", std::process::id()));
        std::fs::create_dir_all(h.join("data")).unwrap();
        std::fs::create_dir_all(h.join("tools")).unwrap();
        std::fs::write(h.join("data/save.bin"), b"player").unwrap();
        std::fs::write(h.join("tools/gone.py"), b"old").unwrap();
        std::fs::write(h.join("setup-files.txt"), "tools/gone.py\ndata/save.bin\n").unwrap();
        let n = super::install_source(std::path::Path::new(&zip), &h, "abc1234").unwrap();
        assert!(n > 100, "{n} files");
        assert!(h.join("tools/setup.py").is_file());
        assert!(!h.join("tools/gone.py").exists(), "a file the new version dropped stays");
        assert_eq!(std::fs::read(h.join("data/save.bin")).unwrap(), b"player");
        assert!(std::fs::read_to_string(h.join("VERSION.txt")).unwrap().contains("(commit abc1234)"));
        let listing = std::fs::read_to_string(h.join("setup-files.txt")).unwrap();
        assert!(listing.lines().all(|l| !l.starts_with("data/") && !l.starts_with(".github/")));
        assert!(super::installed_commit(&h).as_deref() == Some("abc1234"));
        std::fs::remove_dir_all(&h).unwrap();
    }
}

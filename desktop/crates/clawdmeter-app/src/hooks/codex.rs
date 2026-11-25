//! Codex lifecycle hooks. Codex reads `<codex home>/hooks.json`; each command
//! calls `clawdmeter codex-hook <event>` and reuses the desktop event inbox.

use std::fs;
use std::io::{self, IsTerminal as _, Read as _};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde_json::{json, Map, Value};

use super::install::{basename, first_word, quote_exe, HookError, HookStatus};
use super::{Action, Mode};

/// Events the app observes. Approval is an alert; prompt and tool use also
/// clear the alert for the same Codex session.
pub const EVENTS: [&str; 4] = ["UserPromptSubmit", "PostToolUse", "PermissionRequest", "Stop"];

fn command(exe: &Path, event: &str) -> String {
    format!("{} codex-hook {event}", quote_exe(exe))
}

fn managed(command: &str) -> bool {
    let Some((exe, rest)) = first_word(command) else { return false };
    let args: Vec<&str> = rest.split_whitespace().collect();
    if basename(&exe).starts_with("clawdmeter")
        && matches!(args.as_slice(), ["codex-hook", event] if EVENTS.contains(event))
    {
        return true;
    }
    if !matches!(basename(&exe).as_str(), "python" | "python3" | "python3.12") {
        return false;
    }
    let Some((script, rest)) = first_word(rest) else { return false };
    basename(&script) == "codex_clawdmeter_hook.py" && EVENTS.contains(&rest.trim())
}

fn parse(text: Option<&str>) -> Result<Map<String, Value>, HookError> {
    match text.map(str::trim) {
        None | Some("") => Ok(Map::new()),
        Some(s) => match serde_json::from_str::<Value>(s).map_err(|e| HookError::Parse(e.to_string()))? {
            Value::Object(m) => Ok(m),
            _ => Err(HookError::Shape("the top level isn't an object")),
        },
    }
}

fn strip(root: &mut Map<String, Value>) -> Result<(), HookError> {
    let Some(hooks) = root.get_mut("hooks") else { return Ok(()) };
    let Some(hooks) = hooks.as_object_mut() else { return Err(HookError::Shape("\"hooks\" isn't an object")) };
    for groups in hooks.values_mut() {
        let Some(groups) = groups.as_array_mut() else { continue };
        for group in groups.iter_mut() {
            if let Some(entries) = group.get_mut("hooks").and_then(Value::as_array_mut) {
                entries.retain(|entry| !entry.get("command").and_then(Value::as_str).is_some_and(managed));
            }
        }
        groups.retain(|g| g.get("hooks").and_then(Value::as_array).is_none_or(|a| !a.is_empty()));
    }
    hooks.retain(|_, groups| groups.as_array().is_none_or(|a| !a.is_empty()));
    if hooks.is_empty() {
        root.remove("hooks");
    }
    Ok(())
}

fn render(root: Map<String, Value>) -> String {
    serde_json::to_string_pretty(&Value::Object(root)).expect("JSON object").to_string() + "\n"
}

/// Add the four native hooks while retaining every unrelated Codex hook.
pub fn merge(text: Option<&str>, exe: &Path) -> Result<String, HookError> {
    let mut root = parse(text)?;
    strip(&mut root)?;
    let hooks = root
        .entry("hooks")
        .or_insert_with(|| json!({}))
        .as_object_mut()
        .ok_or(HookError::Shape("\"hooks\" isn't an object"))?;
    for event in EVENTS {
        let groups = hooks
            .entry(event)
            .or_insert_with(|| json!([]))
            .as_array_mut()
            .ok_or(HookError::Shape("a hook event isn't a list"))?;
        groups.push(json!({"hooks": [{"type": "command", "command": command(exe, event), "timeout": 5}]}));
    }
    Ok(render(root))
}

/// Remove only Clawdmeter's Codex hooks.
pub fn remove(text: Option<&str>) -> Result<String, HookError> {
    let mut root = parse(text)?;
    strip(&mut root)?;
    Ok(render(root))
}

/// Status of a Codex account's hooks.json.
pub fn status(text: Option<&str>, exe: &Path) -> HookStatus {
    let root = match parse(text) {
        Ok(r) => r,
        Err(e) => return HookStatus::Unreadable(e.to_string()),
    };
    let Some(hooks) = root.get("hooks") else { return HookStatus::Missing };
    let Some(hooks) = hooks.as_object() else {
        return HookStatus::Unreadable("\"hooks\" isn't an object".into());
    };
    let commands: Vec<&str> = hooks
        .values()
        .filter_map(Value::as_array)
        .flatten()
        .filter_map(|g| g.get("hooks").and_then(Value::as_array))
        .flatten()
        .filter_map(|h| h.get("command").and_then(Value::as_str))
        .filter(|c| managed(c))
        .collect();
    if commands.is_empty() {
        return HookStatus::Missing;
    }
    if commands.iter().any(|c| c.contains("codex_clawdmeter_hook.py")) {
        return HookStatus::Legacy;
    }
    if let Some(other) = commands.iter().find(|c| !EVENTS.iter().any(|e| **c == command(exe, e))) {
        return HookStatus::Stale((*other).to_owned());
    }
    if EVENTS.iter().all(|e| {
        hooks
            .get(*e)
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter_map(|g| g.get("hooks").and_then(Value::as_array))
            .flatten()
            .any(|h| h.get("command").and_then(Value::as_str) == Some(command(exe, e).as_str()))
    }) {
        HookStatus::Installed
    } else {
        HookStatus::Partial
    }
}

/// Read the Codex hook JSON and forward only its event name and session ID.
/// `Stop` writes `{}` so Codex sees a valid, empty hook response.
pub fn main(event: Option<&str>) -> i32 {
    let mut input = String::new();
    if !io::stdin().is_terminal() {
        let _ = io::stdin().lock().read_to_string(&mut input);
    }
    let state = clawdmeter_core::Paths::for_user().state_dir;
    let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    if let (Some(event), Ok(input)) = (event, serde_json::from_str::<Value>(&input)) {
        let session = input.get("session_id").and_then(Value::as_str).unwrap_or("");
        if EVENTS.contains(&event)
            && !session.is_empty()
            && session != "."
            && session != ".."
            && !session.contains(['/', '\\', '\0', '\r', '\n'])
        {
            let (name, mode, extra) = match event {
                "UserPromptSubmit" | "PostToolUse" => (event, Mode { action: Action::Clear, log_too: true }, json!({})),
                "PermissionRequest" => (
                    "Notification",
                    Mode { action: Action::Alert, log_too: true },
                    json!({"notification_type": "permission_prompt", "message": "Codex needs permission"}),
                ),
                "Stop" => (event, Mode { action: Action::Event, log_too: false }, json!({})),
                _ => unreachable!(),
            };
            let mut forwarded = json!({"hook_event_name": name, "session_id": session});
            if let (Some(to), Some(from)) = (forwarded.as_object_mut(), extra.as_object()) {
                to.extend(from.clone());
            }
            super::run(mode, &forwarded.to_string(), &state, now);
        }
    }
    if event == Some("Stop") {
        println!("{{}}");
    }
    0
}

/// The user-level hooks file for a Codex account.
pub fn path(codex_dir: &Path) -> PathBuf {
    codex_dir.join("hooks.json")
}

/// Read the current hook status from disk.
pub fn dir_status(codex_dir: &Path, exe: &Path) -> HookStatus {
    match fs::read_to_string(path(codex_dir)) {
        Ok(s) => status(Some(&s), exe),
        Err(e) if e.kind() == io::ErrorKind::NotFound => HookStatus::Missing,
        Err(e) => HookStatus::Unreadable(e.to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn migrates_bridge_and_preserves_other_hooks() {
        let old = r#"{"hooks":{"Stop":[{"hooks":[{"type":"command","command":"/usr/bin/python3 /tmp/codex_clawdmeter_hook.py Stop"},{"type":"command","command":"my-stop-hook"}]}]}}"#;
        let exe = Path::new("/Applications/Clawdmeter.app/Contents/MacOS/clawdmeter");
        let next = merge(Some(old), exe).unwrap();
        assert!(next.contains(" codex-hook Stop"));
        assert!(!next.contains("codex_clawdmeter_hook.py"));
        assert!(next.contains("my-stop-hook"));
        assert_eq!(status(Some(&next), exe), HookStatus::Installed);
        let removed = remove(Some(&next)).unwrap();
        assert!(removed.contains("my-stop-hook"));
        assert!(!removed.contains(" codex-hook "));
    }
}

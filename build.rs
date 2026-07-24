use std::path::Path;

fn main() {
    // embuild: required for esp-idf-sys link patches
    embuild::build::CfgArgs::output_propagated("ESP_IDF").ok();
    embuild::build::LinkArgs::output_propagated("ESP_IDF").ok();

    // Instruct Cargo to rerun this script if config.toml changes.
    println!("cargo:rerun-if-changed=config.toml");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=partitions_ota_4m.csv");
    println!("cargo:rerun-if-changed=partitions_ota_16m.csv");
    println!("cargo:rerun-if-env-changed=SMSGATE_FLASH_LAYOUT");
    println!("cargo:rerun-if-env-changed=SMSGATE_APPLY_COMPILED_CONFIG");

    // ESP-IDF's cmake resolves CONFIG_PARTITION_TABLE_CUSTOM_FILENAME relative
    // to its project root, which for esp-idf-sys is <OUT_DIR>/../../out/.
    // Copy our CSV there so the build can find it.
    if let Ok(out_dir) = std::env::var("OUT_DIR") {
        let layout = selected_flash_layout();
        let src_name = match layout {
            "4m" => "partitions_ota_4m.csv",
            "16m" => "partitions_ota_16m.csv",
            other => panic!("unsupported SMSGATE_FLASH_LAYOUT={other}; expected 4m or 16m"),
        };
        println!("cargo:warning=smsgate flash layout: {layout} ({src_name})");
        let src = Path::new(src_name);
        // OUT_DIR = .../build/smsgate-<hash>/out  →  we need .../build/esp-idf-sys-<hash>/out/
        // but we can't predict the esp-idf-sys hash. Instead, copy to OUT_DIR's grandparent's
        // sibling. Easier: just search for the esp-idf-sys out dir.
        let build_dir = Path::new(&out_dir).parent().unwrap().parent().unwrap();
        if src.exists() {
            for e in std::fs::read_dir(build_dir).into_iter().flatten().flatten() {
                let name = e.file_name();
                if name.to_string_lossy().starts_with("esp-idf-sys-") && e.path().is_dir() {
                    let dst = e.path().join("out").join("partitions_ota.csv");
                    if let Some(parent) = dst.parent() {
                        let _ = std::fs::create_dir_all(parent);
                    }
                    let _ = std::fs::copy(src, &dst);
                }
            }
        }
    }
    println!("cargo::rustc-check-cfg=cfg(locale_zh)");
    println!("cargo::rustc-check-cfg=cfg(esp32s3)");

    let config_path = Path::new("config.toml");
    let apply_compiled_config = match std::env::var("SMSGATE_APPLY_COMPILED_CONFIG") {
        Ok(value) => parse_bool_env(&value).unwrap_or_else(|| {
            panic!("SMSGATE_APPLY_COMPILED_CONFIG must be 1/0, true/false, yes/no, or on/off")
        }),
        Err(_) => config_path.exists(),
    };
    println!("cargo:rustc-env=CFG_APPLY_COMPILED_CONFIG={apply_compiled_config}");
    if !config_path.exists() {
        println!(
            "cargo:warning=config.toml not found. \
             Copy config.toml.example to config.toml and fill in your credentials."
        );
        // Emit empty-string placeholders so the crate still compiles; the
        // device will fail at runtime when it tries to connect.
        emit_empty_defaults();
        emit_git_commit();
        return;
    }

    let config_str = std::fs::read_to_string(config_path).expect("Failed to read config.toml");

    let config: toml::Table = config_str.parse().expect("config.toml is not valid TOML");

    let get = |section: &str, key: &str| -> String {
        config
            .get(section)
            .and_then(|s| s.get(key))
            .and_then(|v| {
                v.as_str()
                    .map(|s| s.to_string())
                    .or_else(|| v.as_integer().map(|i| i.to_string()))
                    .or_else(|| v.as_float().map(|f| f.to_string()))
                    .or_else(|| v.as_bool().map(|b| b.to_string()))
            })
            .unwrap_or_default()
    };

    println!("cargo:rustc-env=CFG_WIFI_SSID={}", get("wifi", "ssid"));
    println!(
        "cargo:rustc-env=CFG_WIFI_PASSWORD={}",
        get("wifi", "password")
    );
    println!("cargo:rustc-env=CFG_IM_BACKEND={}", get("im", "backend"));
    println!(
        "cargo:rustc-env=CFG_IM_BOT_TOKEN={}",
        get("im", "bot_token")
    );
    println!("cargo:rustc-env=CFG_IM_CHAT_ID={}", get("im", "chat_id"));
    let trusted_chat_ids = config
        .get("im")
        .and_then(|im| im.get("trusted_chat_ids"))
        .and_then(|v| v.as_array())
        .map(|ids| {
            let ids: Vec<String> = ids
                .iter()
                .filter_map(|id| id.as_integer().map(|id| id.to_string()))
                .collect();
            format!("[{}]", ids.join(","))
        })
        .unwrap_or_else(|| "[]".to_string());
    println!(
        "cargo:rustc-env=CFG_IM_TRUSTED_CHAT_IDS={}",
        trusted_chat_ids
    );
    println!(
        "cargo:rustc-env=CFG_MODEM_UART_TX={}",
        get("modem", "uart_tx")
    );
    println!(
        "cargo:rustc-env=CFG_MODEM_UART_RX={}",
        get("modem", "uart_rx")
    );
    println!(
        "cargo:rustc-env=CFG_MODEM_UART_BAUD={}",
        get("modem", "uart_baud")
    );
    println!(
        "cargo:rustc-env=CFG_MODEM_PWRKEY={}",
        get("modem", "pwrkey")
    );
    let cellular_data = config
        .get("modem")
        .and_then(|m| m.get("cellular_data"))
        .and_then(|v| v.as_bool())
        .unwrap_or(false);
    println!("cargo:rustc-env=CFG_MODEM_CELLULAR_DATA={}", cellular_data);
    let cellular_fallback = config
        .get("modem")
        .and_then(|m| m.get("cellular_fallback"))
        .and_then(|v| v.as_bool())
        .unwrap_or(false);
    println!(
        "cargo:rustc-env=CFG_CELLULAR_FALLBACK={}",
        cellular_fallback
    );
    let disable_cellular_data = config
        .get("modem")
        .and_then(|m| m.get("disable_cellular_data"))
        .and_then(|v| v.as_bool())
        .unwrap_or(!cellular_data && !cellular_fallback);
    println!(
        "cargo:rustc-env=CFG_MODEM_DISABLE_CELLULAR_DATA={}",
        disable_cellular_data
    );
    println!("cargo:rustc-env=CFG_MODEM_APN={}", get("modem", "apn"));
    println!(
        "cargo:rustc-env=CFG_MODEM_APN_USER={}",
        get("modem", "apn_user")
    );
    println!(
        "cargo:rustc-env=CFG_MODEM_APN_PASS={}",
        get("modem", "apn_pass")
    );
    println!(
        "cargo:rustc-env=CFG_MODEM_SIM_PIN={}",
        get("modem", "sim_pin")
    );
    println!(
        "cargo:rustc-env=CFG_BRIDGE_MAX_FAILURES={}",
        get("bridge", "max_failures_before_reboot")
    );
    println!(
        "cargo:rustc-env=CFG_BRIDGE_POLL_INTERVAL_MS={}",
        get("bridge", "poll_interval_ms")
    );
    println!(
        "cargo:rustc-env=CFG_BRIDGE_WATCHDOG_SEC={}",
        get("bridge", "watchdog_timeout_sec")
    );

    // Serialize [[sink]] array as JSON for runtime parsing
    let sinks_json = config
        .get("sink")
        .and_then(|v| v.as_array())
        .map(|arr| {
            let entries: Vec<String> = arr
                .iter()
                .filter_map(|entry| {
                    let t = entry.get("type")?.as_str()?;
                    let url = entry.get("url")?.as_str()?;
                    Some(format!(r#"{{"type":"{}","url":"{}"}}"#, t, url))
                })
                .collect();
            format!("[{}]", entries.join(","))
        })
        .unwrap_or_else(|| "[]".to_string());
    println!("cargo:rustc-env=CFG_SINKS={}", sinks_json);

    // OTA config
    println!("cargo:rustc-env=CFG_OTA_URL={}", get("ota", "url"));
    println!("cargo:rustc-env=CFG_OTA_CONFIRM={}", get("ota", "confirm"));

    if get("ui", "locale") == "zh" {
        println!("cargo:rustc-cfg=locale_zh");
    }

    emit_git_commit();
}

fn selected_flash_layout() -> &'static str {
    if let Ok(layout) = std::env::var("SMSGATE_FLASH_LAYOUT") {
        return match layout.to_ascii_lowercase().as_str() {
            "4m" | "4mb" => "4m",
            "16m" | "16mb" => "16m",
            _ => panic!("invalid SMSGATE_FLASH_LAYOUT={layout}"),
        };
    }

    // The checked-in S3 sdkconfig is the board identity used by the current
    // Windows build flow. An explicit SMSGATE_FLASH_LAYOUT always wins.
    let defaults = std::env::var("ESP_IDF_SDKCONFIG_DEFAULTS").unwrap_or_default();
    if defaults.to_ascii_lowercase().contains("esp32s3") {
        "16m"
    } else {
        "4m"
    }
}

fn parse_bool_env(value: &str) -> Option<bool> {
    match value.trim().to_ascii_lowercase().as_str() {
        "1" | "true" | "yes" | "on" => Some(true),
        "0" | "false" | "no" | "off" => Some(false),
        _ => None,
    }
}

fn emit_git_commit() {
    let commit = std::process::Command::new("git")
        .args(["rev-parse", "--short", "HEAD"])
        .output()
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|| "unknown".to_string());
    println!("cargo:rustc-env=CFG_GIT_COMMIT={}", commit);
    // Rerun on branch switch (.git/HEAD) or new commit on current branch
    // (.git/refs/heads/<branch> or .git/packed-refs after gc).
    println!("cargo:rerun-if-changed=.git/HEAD");
    println!("cargo:rerun-if-changed=.git/refs/heads/main");
    println!("cargo:rerun-if-changed=.git/packed-refs");
}

fn emit_empty_defaults() {
    for key in &[
        "CFG_WIFI_SSID",
        "CFG_WIFI_PASSWORD",
        "CFG_IM_BACKEND",
        "CFG_IM_BOT_TOKEN",
        "CFG_IM_CHAT_ID",
        "CFG_IM_TRUSTED_CHAT_IDS",
    ] {
        println!("cargo:rustc-env={}=", key);
    }
    println!("cargo:rustc-env=CFG_MODEM_UART_TX=26");
    println!("cargo:rustc-env=CFG_MODEM_UART_RX=27");
    println!("cargo:rustc-env=CFG_MODEM_UART_BAUD=115200");
    println!("cargo:rustc-env=CFG_MODEM_PWRKEY=4");
    println!("cargo:rustc-env=CFG_MODEM_CELLULAR_DATA=false");
    println!("cargo:rustc-env=CFG_CELLULAR_FALLBACK=false");
    println!("cargo:rustc-env=CFG_MODEM_DISABLE_CELLULAR_DATA=true");
    println!("cargo:rustc-env=CFG_MODEM_APN=");
    println!("cargo:rustc-env=CFG_MODEM_APN_USER=");
    println!("cargo:rustc-env=CFG_MODEM_APN_PASS=");
    println!("cargo:rustc-env=CFG_MODEM_SIM_PIN=");
    println!("cargo:rustc-env=CFG_BRIDGE_MAX_FAILURES=8");
    println!("cargo:rustc-env=CFG_BRIDGE_POLL_INTERVAL_MS=3000");
    println!("cargo:rustc-env=CFG_BRIDGE_WATCHDOG_SEC=120");
    println!("cargo:rustc-env=CFG_SINKS=[]");
    println!("cargo:rustc-env=CFG_OTA_URL=");
    println!("cargo:rustc-env=CFG_OTA_CONFIRM=auto");
}

//! OTA (Over-The-Air) firmware update support.
//!
//! Uses ESP-IDF's dual-partition OTA mechanism: download a new firmware binary
//! over HTTPS, write it to the inactive partition, reboot, then confirm or
//! rollback.

#[cfg(feature = "esp32")]
use esp_idf_svc::ota::EspOta;
#[cfg(feature = "esp32")]
use esp_idf_svc::tls::{EspTls, InternalSocket};

#[cfg(feature = "esp32")]
const CHUNK_BUF: usize = 4096;
#[cfg(feature = "esp32")]
const READ_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(120);
#[cfg(feature = "esp32")]
const MAX_REDIRECTS: u8 = 3;

#[derive(Debug)]
pub enum OtaError {
    Disabled,
    Http(String),
    Flash(String),
}

impl core::fmt::Display for OtaError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            OtaError::Disabled => write!(f, "OTA disabled (url is empty)"),
            OtaError::Http(s) => write!(f, "HTTP: {}", s),
            OtaError::Flash(s) => write!(f, "Flash: {}", s),
        }
    }
}

/// Returns true if OTA is configured (URL is non-empty).
pub fn is_enabled() -> bool {
    !crate::config::Config::OTA_URL.is_empty()
}

/// Returns true if confirm mode is "manual".
pub fn is_manual_confirm() -> bool {
    crate::config::Config::OTA_CONFIRM.eq_ignore_ascii_case("manual")
}

/// Mark the currently running partition as valid (prevents rollback).
#[cfg(feature = "esp32")]
pub fn confirm_running() -> Result<(), OtaError> {
    let mut ota = EspOta::new().map_err(|e| OtaError::Flash(format!("{}", e)))?;
    ota.mark_running_slot_valid()
        .map_err(|e| OtaError::Flash(format!("{}", e)))
}

/// Returns the version string of the currently running firmware.
#[cfg(feature = "esp32")]
pub fn running_version() -> String {
    match EspOta::new() {
        Ok(ota) => match ota.get_running_slot() {
            Ok(slot) => slot
                .firmware
                .map(|f| f.version.to_string())
                .unwrap_or_else(|| "unknown".into()),
            Err(_) => "unknown".into(),
        },
        Err(_) => "unknown".into(),
    }
}

/// Download firmware from the configured URL and flash it to the next OTA slot.
/// On success, the caller should reboot to activate the new firmware.
#[cfg(feature = "esp32")]
pub fn perform_update<F>(on_progress: F) -> Result<(), OtaError>
where
    F: Fn(usize, Option<usize>),
{
    if !is_enabled() {
        return Err(OtaError::Disabled);
    }

    let url = crate::config::Config::OTA_URL;
    let (host, port, path, use_tls) = parse_url(url)?;

    let (final_host, final_port, final_path, final_tls) =
        follow_redirects(&host, port, &path, use_tls)?;

    let mut tls = connect_tls(&final_host, final_port, final_tls)?;

    let request = format!(
        "GET {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
        final_path, final_host
    );
    tls.write_all(request.as_bytes())
        .map_err(|e| OtaError::Http(format!("write: {}", e)))?;

    let (status, content_length, header_remainder) = read_headers(&mut tls)?;
    if status < 200 || status >= 300 {
        return Err(OtaError::Http(format!("HTTP {}", status)));
    }

    let mut ota = EspOta::new().map_err(|e| OtaError::Flash(format!("{}", e)))?;
    let mut update = ota
        .initiate_update()
        .map_err(|e| OtaError::Flash(format!("initiate: {}", e)))?;

    let mut written: usize = 0;

    if !header_remainder.is_empty() {
        update
            .write(&header_remainder)
            .map_err(|e| OtaError::Flash(format!("write: {}", e)))?;
        written += header_remainder.len();
        on_progress(written, content_length);
    }

    let mut buf = [0u8; CHUNK_BUF];
    let deadline = std::time::Instant::now() + READ_TIMEOUT;

    loop {
        if std::time::Instant::now() > deadline {
            return Err(OtaError::Http("download timeout".into()));
        }
        if let Some(cl) = content_length {
            if written >= cl {
                break;
            }
        }
        match tls.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                update
                    .write(&buf[..n])
                    .map_err(|e| OtaError::Flash(format!("write: {}", e)))?;
                written += n;
                on_progress(written, content_length);
            }
            Err(e) => {
                if written > 0 && content_length.map_or(true, |cl| written >= cl) {
                    break;
                }
                return Err(OtaError::Http(format!("read: {}", e)));
            }
        }
    }

    if let Some(cl) = content_length {
        if written < cl {
            return Err(OtaError::Http(format!(
                "incomplete: got {} of {} bytes",
                written, cl
            )));
        }
    }

    update
        .complete()
        .map_err(|e| OtaError::Flash(format!("complete: {}", e)))?;

    log::info!("[ota] flashed {} bytes to next slot", written);
    Ok(())
}

#[cfg(feature = "esp32")]
fn parse_url(url: &str) -> Result<(String, u16, String, bool), OtaError> {
    let (scheme, rest) = url
        .split_once("://")
        .ok_or_else(|| OtaError::Http("URL must start with http:// or https://".into()))?;
    let use_tls = scheme == "https";
    let default_port: u16 = if use_tls { 443 } else { 80 };

    let (host_port, path) = match rest.find('/') {
        Some(i) => (&rest[..i], rest[i..].to_string()),
        None => (rest, "/".to_string()),
    };

    let (host, port) = match host_port.rsplit_once(':') {
        Some((h, p)) => (h.to_string(), p.parse::<u16>().unwrap_or(default_port)),
        None => (host_port.to_string(), default_port),
    };

    Ok((host, port, path, use_tls))
}

#[cfg(feature = "esp32")]
fn connect_tls(host: &str, port: u16, use_tls: bool) -> Result<EspTls<InternalSocket>, OtaError> {
    let conf = if use_tls {
        esp_idf_svc::tls::Config::default()
    } else {
        esp_idf_svc::tls::Config::default()
    };
    let mut tls = EspTls::new().map_err(|e| OtaError::Http(format!("TLS init: {}", e)))?;
    if use_tls {
        tls.connect(host, port, &conf)
            .map_err(|e| OtaError::Http(format!("TLS connect: {}", e)))?;
    } else {
        tls.connect(host, port, &esp_idf_svc::tls::Config::default())
            .map_err(|e| OtaError::Http(format!("TCP connect: {}", e)))?;
    }
    Ok(tls)
}

/// Follow HTTP 3xx redirects (up to MAX_REDIRECTS).
#[cfg(feature = "esp32")]
fn follow_redirects(
    host: &str,
    port: u16,
    path: &str,
    use_tls: bool,
) -> Result<(String, u16, String, bool), OtaError> {
    let mut cur_host = host.to_string();
    let mut cur_port = port;
    let mut cur_path = path.to_string();
    let mut cur_tls = use_tls;

    for _ in 0..MAX_REDIRECTS {
        let mut tls = connect_tls(&cur_host, cur_port, cur_tls)?;
        let request = format!(
            "HEAD {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
            cur_path, cur_host
        );
        tls.write_all(request.as_bytes())
            .map_err(|e| OtaError::Http(format!("write: {}", e)))?;

        let (status, _, _) = read_headers(&mut tls)?;
        if (300..400).contains(&status) {
            if let Some(loc) = find_header_in_last_response("location") {
                let (h, p, pa, t) = parse_url(&loc)?;
                cur_host = h;
                cur_port = p;
                cur_path = pa;
                cur_tls = t;
                continue;
            }
        }
        return Ok((cur_host, cur_port, cur_path, cur_tls));
    }

    Ok((cur_host, cur_port, cur_path, cur_tls))
}

/// Read HTTP response headers, returning (status_code, content_length, leftover_body_bytes).
#[cfg(feature = "esp32")]
fn read_headers(
    tls: &mut EspTls<InternalSocket>,
) -> Result<(u16, Option<usize>, Vec<u8>), OtaError> {
    let mut raw = Vec::with_capacity(2048);
    let mut buf = [0u8; 512];
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);

    loop {
        if std::time::Instant::now() > deadline {
            return Err(OtaError::Http("header read timeout".into()));
        }
        let n = tls
            .read(&mut buf)
            .map_err(|e| OtaError::Http(format!("read: {}", e)))?;
        if n == 0 {
            return Err(OtaError::Http("connection closed before headers".into()));
        }
        raw.extend_from_slice(&buf[..n]);
        if let Some(pos) = find_header_end(&raw) {
            let header_str = String::from_utf8_lossy(&raw[..pos]).to_string();
            let remainder = raw[pos + 4..].to_vec(); // skip \r\n\r\n

            let status = parse_status(&header_str)?;
            let content_length = header_str
                .lines()
                .find(|l| l.to_lowercase().starts_with("content-length:"))
                .and_then(|l| l.splitn(2, ':').nth(1))
                .and_then(|v| v.trim().parse().ok());

            // Stash the Location header for redirect following
            if let Some(loc) = header_str
                .lines()
                .find(|l| l.to_lowercase().starts_with("location:"))
                .and_then(|l| l.splitn(2, ':').nth(1))
            {
                LAST_LOCATION.lock().unwrap().replace(loc.trim().to_string());
            } else {
                LAST_LOCATION.lock().unwrap().take();
            }

            return Ok((status, content_length, remainder));
        }
    }
}

#[cfg(feature = "esp32")]
static LAST_LOCATION: std::sync::Mutex<Option<String>> = std::sync::Mutex::new(None);

#[cfg(feature = "esp32")]
fn find_header_in_last_response(_name: &str) -> Option<String> {
    LAST_LOCATION.lock().unwrap().take()
}

#[cfg(feature = "esp32")]
fn find_header_end(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

#[cfg(feature = "esp32")]
fn parse_status(header: &str) -> Result<u16, OtaError> {
    // "HTTP/1.1 200 OK" → 200
    header
        .lines()
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        .and_then(|code| code.parse().ok())
        .ok_or_else(|| OtaError::Http("cannot parse HTTP status".into()))
}

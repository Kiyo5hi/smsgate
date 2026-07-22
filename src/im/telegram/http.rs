//! HTTPS client for Telegram API over ESP-IDF TLS.

use super::types::{ApiResult, TelegramFile};
use esp_idf_svc::tls::{EspTls, InternalSocket, KeepAliveConfig, X509};
use std::time::Duration;

const HOST: &str = "api.telegram.org";
const PORT: u16 = 443;
const READ_TIMEOUT: Duration = Duration::from_secs(30);
// SO_RCVTIMEO for each tls.read() call. Long-poll sends timeout=3 to Telegram (Telegram
// responds within 3 s). The default 4 s gives only 1 s of network margin; any jitter
// causes spurious EAGAIN → reconnect cycles that accumulate to the 5-min stale alert.
const TLS_TIMEOUT_MS: u32 = 20_000;

/// TLS-backed HTTPS client for api.telegram.org.
pub struct TelegramHttpClient {
    tls: EspTls<InternalSocket>,
}

fn make_tls_config(ca_bundle: Option<&'static [u8]>) -> esp_idf_svc::tls::Config<'static> {
    esp_idf_svc::tls::Config {
        ca_cert: ca_bundle.map(|b| X509::pem_until_nul(b)),
        timeout_ms: TLS_TIMEOUT_MS,
        keep_alive_cfg: Some(KeepAliveConfig {
            enable: true,
            idle: Duration::from_secs(60),
            interval: Duration::from_secs(10),
            count: 5,
        }),
        ..Default::default()
    }
}

impl TelegramHttpClient {
    /// Create a new client with optional CA bundle for server verification.
    pub fn new(ca_bundle: Option<&'static [u8]>) -> anyhow::Result<Self> {
        let conf = make_tls_config(ca_bundle);
        let mut tls = EspTls::new()?;
        tls.connect(HOST, PORT, &conf)?;
        Ok(TelegramHttpClient { tls })
    }

    /// POST JSON to a Telegram Bot API path; returns the response body.
    ///
    /// On connection-level failure (server closed keep-alive, timeout, etc.),
    /// reconnects once and retries automatically.
    pub fn post(&mut self, path: &str, json_body: &str) -> anyhow::Result<String> {
        match self.do_post(path, json_body) {
            Ok(body) => Ok(body),
            Err(e) => {
                log::warn!("[http] request failed ({}), reconnecting…", e);
                self.reconnect()?;
                self.do_post(path, json_body)
            }
        }
    }

    fn do_post(&mut self, path: &str, json_body: &str) -> anyhow::Result<String> {
        let body_bytes = json_body.as_bytes();
        let request = format!(
            "POST {} HTTP/1.1\r\n\
             Host: {}\r\n\
             Content-Type: application/json\r\n\
             Content-Length: {}\r\n\
             Connection: keep-alive\r\n\
             \r\n\
             {}",
            path,
            HOST,
            body_bytes.len(),
            json_body
        );

        self.tls.write_all(request.as_bytes())?;

        let mut response = String::with_capacity(4096);
        let mut buf = [0u8; 1024];
        let deadline = std::time::Instant::now() + READ_TIMEOUT;

        // Read until we have the full headers
        loop {
            if std::time::Instant::now() > deadline {
                anyhow::bail!("read timeout");
            }
            let n = self.tls.read(&mut buf)?;
            if n == 0 {
                // Server closed the connection — trigger reconnect
                anyhow::bail!("connection closed before headers received");
            }
            response.push_str(&String::from_utf8_lossy(&buf[..n]));
            if response.contains("\r\n\r\n") {
                break;
            }
        }

        // Parse Content-Length
        let cl: usize = response
            .lines()
            .find(|l| {
                l.get(..15)
                    .is_some_and(|p| p.eq_ignore_ascii_case("content-length:"))
            })
            .and_then(|l| l.splitn(2, ':').nth(1))
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(0);

        // Find body start
        let body_start = response
            .find("\r\n\r\n")
            .map(|i| i + 4)
            .unwrap_or(response.len());
        let mut body = response[body_start..].to_string();

        // Read remaining body bytes
        while body.len() < cl {
            if std::time::Instant::now() > deadline {
                anyhow::bail!("body read timeout");
            }
            let n = self.tls.read(&mut buf)?;
            if n == 0 {
                break;
            }
            body.push_str(&String::from_utf8_lossy(&buf[..n]));
        }

        Ok(body)
    }

    fn reconnect(&mut self) -> anyhow::Result<()> {
        let conf = make_tls_config(None);
        let mut tls = EspTls::new()?;
        tls.connect(HOST, PORT, &conf)?;
        self.tls = tls;
        Ok(())
    }

    pub fn get_file(&mut self, token: &str, file_id: &str) -> anyhow::Result<TelegramFile> {
        let path = format!("/bot{token}/getFile");
        let body = format!(r#"{{"file_id":"{}"}}"#, super::types::json_escape(file_id));
        let raw = self.post(&path, &body)?;
        let result: ApiResult<TelegramFile> = serde_json::from_str(&raw)?;
        if !result.ok {
            anyhow::bail!(
                "{}",
                result
                    .description
                    .unwrap_or_else(|| "getFile failed".into())
            );
        }
        result
            .result
            .ok_or_else(|| anyhow::anyhow!("getFile returned no result"))
    }

    /// Stream a Telegram file into a consumer without buffering the image.
    pub fn download_file<F>(
        &mut self,
        token: &str,
        file_path: &str,
        mut on_chunk: F,
    ) -> anyhow::Result<usize>
    where
        F: FnMut(&[u8]) -> anyhow::Result<()>,
    {
        self.reconnect()?;
        let path = format!("/file/bot{token}/{file_path}");
        let request = format!("GET {path} HTTP/1.1\r\nHost: {HOST}\r\nConnection: close\r\n\r\n");
        self.tls.write_all(request.as_bytes())?;

        let mut raw = Vec::with_capacity(2048);
        let mut buf = [0u8; 4096];
        let deadline = std::time::Instant::now() + Duration::from_secs(120);
        let header_end = loop {
            if std::time::Instant::now() > deadline {
                anyhow::bail!("file header timeout");
            }
            let count = self.tls.read(&mut buf)?;
            if count == 0 {
                anyhow::bail!("connection closed before file headers");
            }
            raw.extend_from_slice(&buf[..count]);
            if raw.len() > 8192 {
                anyhow::bail!("file response headers too large");
            }
            if let Some(position) = raw.windows(4).position(|window| window == b"\r\n\r\n") {
                break position;
            }
        };
        let headers = String::from_utf8_lossy(&raw[..header_end]);
        let status = headers
            .lines()
            .next()
            .and_then(|line| line.split_whitespace().nth(1))
            .and_then(|value| value.parse::<u16>().ok())
            .unwrap_or(0);
        if !(200..300).contains(&status) {
            anyhow::bail!("file download HTTP {status}");
        }
        if headers.lines().any(|line| {
            line.to_ascii_lowercase()
                .starts_with("transfer-encoding: chunked")
        }) {
            anyhow::bail!("chunked file download unsupported");
        }
        let expected = headers
            .lines()
            .find(|line| {
                line.get(..15)
                    .is_some_and(|prefix| prefix.eq_ignore_ascii_case("content-length:"))
            })
            .and_then(|line| line.split_once(':'))
            .and_then(|(_, value)| value.trim().parse::<usize>().ok());
        let remainder = &raw[header_end + 4..];
        let mut received = remainder.len();
        if !remainder.is_empty() {
            on_chunk(remainder)?;
        }
        while expected.is_none_or(|size| received < size) {
            if std::time::Instant::now() > deadline {
                anyhow::bail!("file download timeout");
            }
            let count = self.tls.read(&mut buf)?;
            if count == 0 {
                break;
            }
            on_chunk(&buf[..count])?;
            received += count;
        }
        if expected.is_some_and(|size| received != size) {
            anyhow::bail!("incomplete file: got {received} of {}", expected.unwrap());
        }
        Ok(received)
    }
}

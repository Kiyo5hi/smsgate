//! Generic webhook sink — POSTs each message as JSON to a configured URL.
//!
//! Each `send_message` opens a fresh TLS connection, sends the payload,
//! and closes. No persistent connection means zero idle RAM overhead.

#[cfg(feature = "esp32")]
use super::{MessageId, MessageSink, MessengerError};

#[cfg(feature = "esp32")]
use esp_idf_svc::tls::{EspTls, InternalSocket};

#[cfg(feature = "esp32")]
pub struct WebhookSink {
    host: String,
    port: u16,
    path: String,
    use_tls: bool,
}

#[cfg(feature = "esp32")]
impl WebhookSink {
    /// Parse a URL like `https://example.com:8443/hook/path` into components.
    pub fn from_url(url: &str) -> Result<Self, MessengerError> {
        let (scheme, rest) = url.split_once("://")
            .ok_or_else(|| MessengerError::Http("webhook URL must start with http:// or https://".into()))?;
        let use_tls = scheme == "https";
        let default_port: u16 = if use_tls { 443 } else { 80 };

        let (host_port, path) = match rest.find('/') {
            Some(i) => (&rest[..i], &rest[i..]),
            None => (rest, "/"),
        };

        let (host, port) = match host_port.rsplit_once(':') {
            Some((h, p)) => (h.to_string(), p.parse::<u16>().unwrap_or(default_port)),
            None => (host_port.to_string(), default_port),
        };

        Ok(WebhookSink { host, port, path: path.to_string(), use_tls })
    }
}

#[cfg(feature = "esp32")]
impl MessageSink for WebhookSink {
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError> {
        let escaped = super::telegram::types::json_escape(text);
        let body = format!(r#"{{"text":"{}"}}"#, escaped);
        let body_bytes = body.as_bytes();

        let request = format!(
            "POST {} HTTP/1.1\r\n\
             Host: {}\r\n\
             Content-Type: application/json\r\n\
             Content-Length: {}\r\n\
             Connection: close\r\n\
             \r\n\
             {}",
            self.path, self.host, body_bytes.len(), body
        );

        if self.use_tls {
            let conf = esp_idf_svc::tls::Config::default();
            let mut tls: EspTls<InternalSocket> = EspTls::new()
                .map_err(|e| MessengerError::Http(format!("TLS init: {}", e)))?;
            tls.connect(&self.host, self.port, &conf)
                .map_err(|e| MessengerError::Http(format!("TLS connect: {}", e)))?;
            tls.write_all(request.as_bytes())
                .map_err(|e| MessengerError::Http(format!("TLS write: {}", e)))?;
            drain_response(&mut tls)?;
        } else {
            use std::io::Write;
            let addr = format!("{}:{}", self.host, self.port);
            let mut stream = std::net::TcpStream::connect(&addr)
                .map_err(|e| MessengerError::Http(format!("TCP connect: {}", e)))?;
            stream.set_write_timeout(Some(std::time::Duration::from_secs(10))).ok();
            stream.write_all(request.as_bytes())
                .map_err(|e| MessengerError::Http(format!("TCP write: {}", e)))?;
        }

        Ok(0)
    }
}

#[cfg(feature = "esp32")]
fn drain_response(tls: &mut EspTls<InternalSocket>) -> Result<(), MessengerError> {
    let mut buf = [0u8; 512];
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
    loop {
        if std::time::Instant::now() > deadline { break; }
        match tls.read(&mut buf) {
            Ok(0) => break,
            Ok(_) => continue,
            Err(_) => break,
        }
    }
    Ok(())
}

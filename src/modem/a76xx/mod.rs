//! A76xx modem driver — ESP32 / UART implementation.

pub mod at;

#[cfg(feature = "esp32")]
pub mod qhttp;
#[cfg(feature = "esp32")]
pub mod sms;

#[cfg(feature = "esp32")]
use std::time::Duration;
#[cfg(feature = "esp32")]
use super::{AtResponse, AtTransport, ModemError, ModemPort, creg_registered};
#[cfg(feature = "esp32")]
use at::HardwareAtPort as AtPort;

/// A76xx modem driver (A7670, A7608, A7672, etc.).
#[cfg(feature = "esp32")]
pub struct A76xxModem {
    port: AtPort,
}

#[cfg(feature = "esp32")]
impl A76xxModem {
    /// Create from an already-configured `AtPort`.
    pub fn new(port: AtPort) -> Self {
        A76xxModem { port }
    }

    pub(crate) fn port_mut(&mut self) -> &mut AtPort {
        &mut self.port
    }

    /// Run the initialisation sequence:
    /// - Echo off, PDU mode, enable CMT URCs, wait for network registration.
    /// - Optionally attach or detach packet-switched service (`AT+CGATT`).
    pub fn init(&mut self, cellular_data: bool) -> Result<(), ModemError> {
        // Probe until the modem responds to AT (up to 15 s).
        // A7670G typically takes 5-10 s after power-on to become responsive.
        let probe_deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
        loop {
            let r = self.send_at(""); // sends "AT\r" — basic liveness check
            if r.is_ok() {
                log::info!("[a76xx] modem responded to AT probe");
                break;
            }
            if std::time::Instant::now() > probe_deadline {
                log::error!("[a76xx] modem did not respond within 30 s");
                return Err(ModemError::Timeout);
            }
            std::thread::sleep(std::time::Duration::from_millis(500));
        }

        let cmds = [
            "E0",              // echo off
            "+CMGF=0",         // PDU mode
            "+CNMI=2,1,0,0,0", // store SMS in modem memory, notify via +CMTI: <mem>,<idx>
            "+CLIP=1",         // caller-line identification
        ];
        for cmd in &cmds {
            let r = self.send_at(cmd)?;
            if r.ok {
                log::info!("[a76xx] init AT{} OK", cmd);
            } else {
                log::warn!("[a76xx] init AT{} ERROR: {}", cmd, r.body.trim());
            }
        }

        // Verify CNMI setting was accepted
        match self.send_at("+CNMI?") {
            Ok(r) if r.ok => log::info!("[a76xx] CNMI: {}", r.body.trim()),
            Ok(r)         => log::warn!("[a76xx] CNMI? error: {}", r.body.trim()),
            Err(_)        => log::warn!("[a76xx] CNMI? timed out"),
        }

        // Query active storage for diagnostics. Non-fatal; some SIM/modem combos
        // return +CMS ERROR here if SMS management isn't supported.
        match self.send_at("+CPMS?") {
            Ok(r) if r.ok  => log::info!("[a76xx] CPMS: {}", r.body.trim()),
            Ok(r)          => log::debug!("[a76xx] CPMS? not supported: {}", r.body.trim()),
            Err(_)         => log::debug!("[a76xx] CPMS? timed out"),
        }

        // Wait for network registration (up to 30 s)
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
        loop {
            let r = self.send_at("+CREG?")?;
            if creg_registered(&r.body) {
                log::info!("[a76xx] network registered");
                break;
            }
            if std::time::Instant::now() > deadline {
                log::warn!("[a76xx] network registration timed out — continuing anyway");
                break;
            }
            std::thread::sleep(std::time::Duration::from_secs(2));
        }

        // Only send AT+CGATT=1 when cellular data is explicitly requested.
        // AT+CGATT=0 (detach) is unreliable on A7670G — the modem frequently
        // doesn't respond within CMD_TIMEOUT, causing a 5 s stall at boot.
        // SMS delivery works without touching CGATT.
        if cellular_data {
            match self.send_at("+CGATT=1") {
                Ok(r) if r.ok => log::info!("[a76xx] cellular data enabled (AT+CGATT=1 OK)"),
                Ok(r)         => log::warn!("[a76xx] AT+CGATT=1: {}", r.body.trim()),
                Err(e)        => log::warn!("[a76xx] AT+CGATT=1 failed: {}", e),
            }
        }
        Ok(())
    }
}

#[cfg(feature = "esp32")]
impl AtTransport for A76xxModem {
    fn send_at(&mut self, cmd: &str) -> Result<AtResponse, ModemError> {
        self.port.send_at(cmd)
    }

    fn poll_urc(&mut self) -> Option<String> {
        self.port.poll_urc()
    }

    fn write_raw(&mut self, data: &[u8]) -> Result<(), ModemError> {
        self.port.write_raw(data)
    }

    fn wait_for_prompt(&mut self, prompt: u8, timeout: Duration) -> bool {
        self.port.wait_for_prompt(prompt, timeout)
    }
}

#[cfg(feature = "esp32")]
impl ModemPort for A76xxModem {
    // send_pdu_sms: default (standard AT+CMGS handshake via AtTransport)
    // hang_up: default (ATH)

    fn post_telegram_https(&mut self, path: &str, json: &str) -> Result<String, ModemError> {
        qhttp::post_json(self, path, json)
    }
}

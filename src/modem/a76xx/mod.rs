//! A76xx modem driver — ESP32 / UART implementation.

pub mod at;
pub mod sms;

use super::{AtResponse, ModemError, ModemPort, creg_registered};
use at::AtPort;

/// A76xx modem driver (A7670, A7608, A7672, etc.).
pub struct A76xxModem {
    port: AtPort,
}

impl A76xxModem {
    /// Create from an already-configured `AtPort`.
    pub fn new(port: AtPort) -> Self {
        A76xxModem { port }
    }

    /// Run the initialisation sequence:
    /// - Echo off, PDU mode, enable CMT URCs, wait for network registration.
    pub fn init(&mut self) -> Result<(), ModemError> {
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
        Ok(())
    }

    /// Query signal strength and operator; update the supplied status.
    pub fn update_status(&mut self) -> super::ModemStatus {
        let mut s = super::ModemStatus::default();
        if let Ok(r) = self.send_at("+CSQ") {
            // +CSQ: 20,0
            if let Some(v) = r.body.strip_prefix("+CSQ: ") {
                let csq: u8 = v.split(',').next().and_then(|x| x.trim().parse().ok()).unwrap_or(super::CSQ_UNKNOWN);
                s.csq = csq;
            }
        }
        if let Ok(r) = self.send_at("+COPS?") {
            // +COPS: 0,0,"China Mobile",7
            if let Some(start) = r.body.find('"') {
                if let Some(end) = r.body[start + 1..].find('"') {
                    s.operator = r.body[start + 1..start + 1 + end].to_string();
                }
            }
        }
        if let Ok(r) = self.send_at("+CREG?") {
            s.registered = creg_registered(&r.body);
        }
        s
    }
}

impl ModemPort for A76xxModem {
    fn send_at(&mut self, cmd: &str) -> Result<AtResponse, ModemError> {
        self.port.send_at(cmd)
    }

    fn poll_urc(&mut self) -> Option<String> {
        self.port.poll_urc()
    }

    fn send_pdu_sms(&mut self, hex: &str, tpdu_len: u8) -> Result<u8, ModemError> {
        sms::send_pdu(&mut self.port, hex, tpdu_len)
    }

    fn hang_up(&mut self) -> Result<(), ModemError> {
        let r = self.send_at("H")?;
        if r.ok { Ok(()) } else { Err(ModemError::AtError("ATH failed".into())) }
    }
}

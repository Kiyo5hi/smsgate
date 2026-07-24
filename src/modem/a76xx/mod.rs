//! A76xx modem driver — ESP32 / UART implementation.

pub mod at;
pub mod sim;

#[cfg(feature = "esp32")]
pub mod qhttp;
#[cfg(feature = "esp32")]
pub mod sms;

#[cfg(feature = "esp32")]
use super::{creg_registered, AtResponse, AtTransport, ModemError, ModemPort};
#[cfg(feature = "esp32")]
use at::HardwareAtPort as AtPort;
#[cfg(feature = "esp32")]
use std::time::Duration;

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
    pub fn init(
        &mut self,
        cellular_data: bool,
        disable_cellular_data: bool,
        sim_pin: &str,
    ) -> Result<(), ModemError> {
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

        let echo = self.send_at("E0")?;
        if !echo.ok {
            log::warn!("[a76xx] init ATE0 ERROR: {}", echo.body.trim());
        }
        sim::ensure_sim_unlocked(self, sim_pin)?;

        for cmd in &["+CMGF=0", "+CLIP=1", "+CTZU=1", "+CTZR=1"] {
            let r = self.send_at(cmd)?;
            if r.ok {
                log::info!("[a76xx] init AT{} OK", cmd);
            } else {
                log::warn!("[a76xx] init AT{} ERROR: {}", cmd, r.body.trim());
            }
        }

        // AT+CNMI=2,1,0,0,0 must succeed for +CMTI notifications to work.
        // On warm reboot the modem resets and its SMS subsystem may not be ready
        // when the AT probe first succeeds. Retry until accepted (up to 30 s).
        let cnmi_deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
        loop {
            match self.send_at("+CNMI=2,1,0,0,0") {
                Ok(r) if r.ok => {
                    log::info!("[a76xx] CNMI set OK");
                    break;
                }
                Ok(r) => log::warn!("[a76xx] CNMI ERROR: {} — retrying", r.body.trim()),
                Err(e) => log::warn!("[a76xx] CNMI timeout: {} — retrying", e),
            }
            if std::time::Instant::now() > cnmi_deadline {
                log::error!(
                    "[a76xx] CNMI never accepted after 30 s — SMS notifications may not work"
                );
                break;
            }
            std::thread::sleep(std::time::Duration::from_secs(2));
        }

        // Verify CNMI setting was accepted
        match self.send_at("+CNMI?") {
            Ok(r) if r.ok => log::info!("[a76xx] CNMI: {}", r.body.trim()),
            Ok(r) => log::warn!("[a76xx] CNMI? error: {}", r.body.trim()),
            Err(_) => log::warn!("[a76xx] CNMI? timed out"),
        }

        // Query active storage for diagnostics. Non-fatal; some SIM/modem combos
        // return +CMS ERROR here if SMS management isn't supported.
        match self.send_at("+CPMS?") {
            Ok(r) if r.ok => log::info!("[a76xx] CPMS: {}", r.body.trim()),
            Ok(r) => log::debug!("[a76xx] CPMS? not supported: {}", r.body.trim()),
            Err(_) => log::debug!("[a76xx] CPMS? timed out"),
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
                Ok(r) => log::warn!("[a76xx] AT+CGATT=1: {}", r.body.trim()),
                Err(e) => log::warn!("[a76xx] AT+CGATT=1 failed: {}", e),
            }
        } else if disable_cellular_data {
            match self.disable_packet_data_contexts() {
                Ok(()) => log::info!("[a76xx] packet data guard enabled"),
                Err(e) => log::warn!("[a76xx] packet data guard init failed: {}", e),
            }
        }
        Ok(())
    }

    fn disable_packet_data_contexts(&mut self) -> Result<(), ModemError> {
        let before = self.send_at("+CGACT?").ok();
        if let Some(response) = before.as_ref().filter(|response| response.ok) {
            log::info!("[a76xx] CGACT before guard: {}", response.body.trim());
        }

        let mut completed = false;
        let mut any_ok = false;
        let mut last_error = None;
        for cmd in [
            "+QIDEACT=1",
            "+QIDEACT=8",
            "+CNACT=0,1",
            "+CNACT=0,8",
            "+CGACT=0,1",
            "+CGACT=0,8",
        ] {
            match self.send_at(cmd) {
                Ok(response) => {
                    completed = true;
                    if response.ok {
                        any_ok = true;
                        log::info!("[a76xx] data guard AT{} OK", cmd);
                    } else {
                        log::debug!(
                            "[a76xx] data guard AT{} returned: {}",
                            cmd,
                            response.body.trim()
                        );
                    }
                }
                Err(error) => {
                    log::warn!("[a76xx] data guard AT{} failed: {}", cmd, error);
                    last_error = Some(error);
                }
            }
        }

        match self.send_at("+CGACT?") {
            Ok(response) if response.ok => {
                log::info!("[a76xx] CGACT after guard: {}", response.body.trim());
                if packet_data_context_active(&response.body) {
                    self.detach_packet_domain(&response.body)
                } else {
                    Ok(())
                }
            }
            Ok(response) => {
                if any_ok || completed {
                    Ok(())
                } else {
                    Err(ModemError::AtError(response.body))
                }
            }
            Err(error) => {
                if any_ok || completed {
                    Ok(())
                } else {
                    Err(last_error.unwrap_or(error))
                }
            }
        }
    }

    fn detach_packet_domain(&mut self, active_contexts: &str) -> Result<(), ModemError> {
        log::warn!(
            "[a76xx] PDP still active after CGACT; detaching packet domain: {}",
            active_contexts.trim()
        );
        match self
            .port
            .send_at_timeout("+CGATT=0", std::time::Duration::from_secs(60))
        {
            Ok(response) if response.ok => {
                log::info!("[a76xx] data guard AT+CGATT=0 OK");
                std::thread::sleep(std::time::Duration::from_secs(1));
            }
            Ok(response) => {
                return Err(ModemError::AtError(format!(
                    "CGATT=0 failed: {}",
                    response.body.trim()
                )));
            }
            Err(error) => return Err(error),
        }

        match self.send_at("+CGATT?") {
            Ok(response) if response.ok => {
                log::info!("[a76xx] CGATT after guard: {}", response.body.trim());
                if packet_domain_detached(&response.body) {
                    return Ok(());
                }
            }
            Ok(response) => log::debug!("[a76xx] CGATT? after guard: {}", response.body.trim()),
            Err(error) => log::warn!("[a76xx] CGATT? after guard failed: {}", error),
        }

        match self.send_at("+CGACT?") {
            Ok(response) if response.ok => {
                log::info!("[a76xx] CGACT after detach: {}", response.body.trim());
                if packet_data_context_active(&response.body) {
                    Err(ModemError::AtError(format!(
                        "packet data still active: {}",
                        response.body.trim()
                    )))
                } else {
                    Ok(())
                }
            }
            Ok(response) => Err(ModemError::AtError(response.body)),
            Err(error) => Err(error),
        }
    }
}

#[cfg(feature = "esp32")]
fn packet_data_context_active(body: &str) -> bool {
    body.lines().any(|line| {
        let Some(rest) = line.trim().strip_prefix("+CGACT:") else {
            return false;
        };
        rest.split(',')
            .nth(1)
            .is_some_and(|state| state.trim() == "1")
    })
}

#[cfg(feature = "esp32")]
fn packet_domain_detached(body: &str) -> bool {
    body.lines().any(|line| {
        let Some(rest) = line.trim().strip_prefix("+CGATT:") else {
            return false;
        };
        rest.trim() == "0"
    })
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

    fn hang_up(&mut self) -> Result<(), ModemError> {
        let r = self.send_at("+CHUP")?;
        if r.ok {
            Ok(())
        } else {
            Err(ModemError::AtError("AT+CHUP failed".into()))
        }
    }

    fn post_telegram_https(&mut self, path: &str, json: &str) -> Result<String, ModemError> {
        qhttp::post_json(self, path, json)
    }

    fn disable_packet_data(&mut self) -> Result<(), ModemError> {
        self.disable_packet_data_contexts()
    }
}

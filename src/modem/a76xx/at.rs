//! Raw AT send/receive over ESP32 UART.

use crate::modem::{AtResponse, ModemError};
use esp_idf_hal::io::Write as EmbeddedWrite;
use esp_idf_hal::uart::UartDriver;
use std::time::{Duration, Instant};

/// FreeRTOS ticks to block in uart.read() waiting for a byte.
/// 10 ticks ≈ 10 ms at the default 1 kHz tick rate.
/// Blocking (rather than NON_BLOCK + sleep) allows the scheduler to run IDLE
/// and other tasks, preventing the Task Watchdog from firing.
const UART_READ_TICKS: u32 = 10;

const CMD_TIMEOUT: Duration = Duration::from_secs(5);
const READLINE_TIMEOUT: Duration = Duration::from_millis(500);

/// Low-level AT command port backed by an ESP-IDF UART driver.
pub struct AtPort {
    uart: UartDriver<'static>,
    urc_buf: std::collections::VecDeque<String>,
}

impl AtPort {
    pub fn new(uart: UartDriver<'static>) -> Self {
        AtPort { uart, urc_buf: std::collections::VecDeque::new() }
    }

    /// Send "AT<cmd>\r" and collect lines until OK/ERROR/timeout.
    pub fn send_at(&mut self, cmd: &str) -> Result<AtResponse, ModemError> {
        // Flush any pending URCs first
        self.drain_urcs();

        let command = format!("AT{}\r", cmd);
        self.uart.write_all(command.as_bytes()).map_err(|_| ModemError::Io)?;

        let deadline = Instant::now() + CMD_TIMEOUT;
        let mut body_lines: Vec<String> = Vec::new();

        loop {
            if Instant::now() > deadline {
                return Err(ModemError::Timeout);
            }
            match self.read_line(READLINE_TIMEOUT) {
                Some(line) => {
                    let line = line.trim().to_string();
                    if line.is_empty() { continue; }
                    if line == "OK" {
                        break;
                    }
                    if line.starts_with("ERROR") || line.starts_with("+CME ERROR") || line.starts_with("+CMS ERROR") {
                        return Ok(AtResponse { body: line, ok: false });
                    }
                    // URC-shaped lines during a command (piggybacked)
                    if urc::is_urc(&line) {
                        self.urc_buf.push_back(line);
                    } else {
                        body_lines.push(line);
                    }
                }
                None => {
                    // No data yet; spin
                }
            }
        }

        Ok(AtResponse {
            body: body_lines.join("\n"),
            ok: true,
        })
    }

    /// Non-blocking: drain one URC line if available.
    pub fn poll_urc(&mut self) -> Option<String> {
        // First check buffered URCs from piggybacked reads
        if let Some(urc) = self.urc_buf.pop_front() {
            return Some(urc);
        }
        // Then try to read a fresh line without blocking
        let line = self.read_line(Duration::from_millis(10))?;
        let line = line.trim().to_string();
        if line.is_empty() { return None; }
        Some(line)
    }

    // ---- private ----

    fn drain_urcs(&mut self) {
        // Use a short non-blocking window: read_line with 20ms timeout collects
        // any buffered data already waiting in the UART FIFO.
        while let Some(line) = self.read_line(Duration::from_millis(20)) {
            let line = line.trim().to_string();
            if !line.is_empty() {
                self.urc_buf.push_back(line);
            }
        }
    }

    fn read_line(&mut self, timeout: Duration) -> Option<String> {
        let deadline = Instant::now() + timeout;
        let mut line = String::new();
        let mut buf = [0u8; 1];
        loop {
            if Instant::now() > deadline {
                if !line.is_empty() { return Some(line); }
                return None;
            }
            // Block for UART_READ_TICKS (≈10 ms) waiting for a byte.
            // This properly yields the CPU to the FreeRTOS IDLE task and other
            // tasks, preventing the Task Watchdog from triggering on IDLE.
            let n = self.uart.read(&mut buf, UART_READ_TICKS).unwrap_or(0);
            if n == 0 {
                continue; // timed out, check deadline and retry
            }
            let c = buf[0];
            if c == b'\n' {
                return Some(line);
            }
            if c != b'\r' {
                line.push(c as char);
            }
        }
    }

    /// Write raw bytes to UART (used for AT+CMGS PDU send).
    pub fn write_raw(&mut self, data: &[u8]) -> Result<(), ModemError> {
        self.uart.write_all(data).map_err(|_| ModemError::Io)
    }

    /// Read until prompt or timeout (used for AT+CMGS '>' prompt).
    pub fn wait_for_prompt(&mut self, prompt: u8, timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        let mut buf = [0u8; 1];
        loop {
            if Instant::now() > deadline { return false; }
            let n = self.uart.read(&mut buf, 0).unwrap_or(0);
            if n > 0 && buf[0] == prompt { return true; }
            std::thread::sleep(Duration::from_millis(1));
        }
    }
}

use crate::modem::urc;

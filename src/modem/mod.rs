//! Modem abstraction layer.
//!
//! Trait definitions live here. Concrete implementations under a76xx/.

pub mod urc;

#[cfg(feature = "esp32")]
pub mod a76xx;

use thiserror::Error;

/// Raw AT command response.
#[derive(Debug, Clone)]
pub struct AtResponse {
    /// All lines before the final status line, joined.
    pub body: String,
    /// True if the response ended with OK; false for ERROR / CME ERROR.
    pub ok: bool,
}

/// Errors from the modem layer.
#[derive(Debug, Error)]
pub enum ModemError {
    #[error("timeout waiting for response")]
    Timeout,
    #[error("modem returned ERROR: {0}")]
    AtError(String),
    #[error("UART write failed")]
    Io,
    #[error("modem not ready")]
    NotReady,
}

/// Signal strength snapshot.
#[derive(Debug, Clone)]
pub struct ModemStatus {
    /// CSQ value (0–31), 99 = unknown.
    pub csq: u8,
    /// Operator name.
    pub operator: String,
    /// Registration status: true = registered.
    pub registered: bool,
}

impl Default for ModemStatus {
    fn default() -> Self {
        ModemStatus { csq: 99, operator: String::new(), registered: false }
    }
}

/// Abstracts the modem's serial port and AT command interface.
pub trait ModemPort {
    /// Send an AT command suffix (without "AT" prefix); return the response.
    fn send_at(&mut self, cmd: &str) -> Result<AtResponse, ModemError>;

    /// Non-blocking poll: return a URC line if one is available.
    fn poll_urc(&mut self) -> Option<String>;

    /// Send an SMS in PDU mode; return the message reference number.
    fn send_pdu_sms(&mut self, hex: &str, tpdu_len: u8) -> Result<u8, ModemError>;

    /// Hang up the current call.
    fn hang_up(&mut self) -> Result<(), ModemError>;
}

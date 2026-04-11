//! Board hardware abstraction layer.

#[cfg(feature = "esp32")]
pub mod ta7670x;

use crate::modem::ModemPort;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum BoardError {
    #[error("GPIO init failed: {0}")]
    Gpio(String),
    #[error("UART init failed: {0}")]
    Uart(String),
    #[error("power-on sequence failed")]
    PowerOn,
}

/// Modem chip family.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModemVariant {
    A76xx, // A7670, A7608, A7672, …
}

/// Board-level abstraction: pin layout + power-on sequence.
///
/// Used only in `main.rs` during startup.
/// After `build_modem_port()` returns, everything else uses `ModemPort`.
#[cfg(feature = "esp32")]
pub trait Board {
    fn modem_variant(&self) -> ModemVariant;
    fn uart_tx_pin(&self) -> u8;
    fn uart_rx_pin(&self) -> u8;
    fn uart_baud(&self) -> u32;
    fn pwrkey_pin(&self) -> u8;
    fn reset_pin(&self) -> Option<u8>;

    /// Configure GPIO, perform power-on sequence.
    fn init(
        &self,
        peripherals: &mut esp_idf_hal::peripherals::Peripherals,
    ) -> Result<(), BoardError>;

    /// Build a configured ModemPort.
    fn build_modem_port(
        &self,
        peripherals: &mut esp_idf_hal::peripherals::Peripherals,
    ) -> Result<Box<dyn ModemPort>, BoardError>;
}

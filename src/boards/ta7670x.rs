//! LilyGo T-A7670X board implementation.
//!
//! Pin defaults are from config.toml; can be overridden for other boards
//! simply by creating a new Board impl with different constants.

use super::{Board, BoardError, ModemVariant};
use crate::config::Config;
use crate::modem::{ModemPort, a76xx::{at::AtPort, A76xxModem}};
use std::sync::{Arc, Mutex};
use esp_idf_hal::{
    peripherals::Peripherals,
    uart::{config::Config as UartConfig, UartDriver},
};
use std::time::Duration;

pub struct TA7670X;

impl Board for TA7670X {
    fn modem_variant(&self) -> ModemVariant { ModemVariant::A76xx }
    fn uart_tx_pin(&self) -> u8 { Config::UART_TX }
    fn uart_rx_pin(&self) -> u8 { Config::UART_RX }
    fn uart_baud(&self) -> u32 { Config::UART_BAUD }
    fn pwrkey_pin(&self) -> u8 { Config::PWRKEY_PIN }
    fn reset_pin(&self) -> Option<u8> { None }

    fn init(&self, _peripherals: &mut Peripherals) -> Result<(), BoardError> {
        use esp_idf_hal::gpio::AnyOutputPin;

        // Step 1: Assert BOARD_POWERON_PIN (pin 12) HIGH.
        // This pin drives the modem's power rail. Without it the modem gets no
        // supply voltage and will never respond to AT commands.
        // (LilyGo gpio.h: "The modem power switch must be set to HIGH for the
        //  modem to supply power.")
        let mut poweron = unsafe {
            esp_idf_hal::gpio::PinDriver::output(
                AnyOutputPin::steal(12u8)
            ).map_err(|e| BoardError::Gpio(e.to_string()))?
        };
        poweron.set_high().map_err(|e| BoardError::Gpio(e.to_string()))?;
        std::thread::sleep(Duration::from_millis(200)); // let rail stabilise

        // Step 2: MODEM_RESET_PIN (pin 5) — hard-reset the modem.
        // Sequence from LilyGo C++ reference (MODEM_RESET_LEVEL=HIGH for T-A7670X):
        //   LOW for 100 ms → HIGH for 2600 ms → LOW.
        let mut reset_pin = unsafe {
            esp_idf_hal::gpio::PinDriver::output(
                AnyOutputPin::steal(5u8)
            ).map_err(|e| BoardError::Gpio(e.to_string()))?
        };
        reset_pin.set_low().map_err(|e| BoardError::Gpio(e.to_string()))?;
        std::thread::sleep(Duration::from_millis(100));
        reset_pin.set_high().map_err(|e| BoardError::Gpio(e.to_string()))?;
        std::thread::sleep(Duration::from_millis(2600));
        reset_pin.set_low().map_err(|e| BoardError::Gpio(e.to_string()))?;
        drop(reset_pin);

        // Step 4: PWRKEY pulse — LOW for 100 ms, HIGH for 100 ms, back to LOW.
        // Matches LilyGo C++ reference: LOW→100ms→HIGH→PULSE_WIDTH(100ms)→LOW.
        let mut pwrkey = unsafe {
            esp_idf_hal::gpio::PinDriver::output(
                AnyOutputPin::steal(Config::PWRKEY_PIN)
            ).map_err(|e| BoardError::Gpio(e.to_string()))?
        };
        pwrkey.set_low().map_err(|e| BoardError::Gpio(e.to_string()))?;
        std::thread::sleep(Duration::from_millis(100));
        pwrkey.set_high().map_err(|e| BoardError::Gpio(e.to_string()))?;
        std::thread::sleep(Duration::from_millis(100));
        pwrkey.set_low().map_err(|e| BoardError::Gpio(e.to_string()))?;

        // Step 5: Wait for modem to complete its boot sequence (~3 s).
        std::thread::sleep(Duration::from_millis(3000));

        // Keep poweron pin driven HIGH for the entire program lifetime.
        // Dropping it here would let the GPIO go to a default state — keep it
        // alive via core::mem::forget so the rail stays asserted.
        core::mem::forget(poweron);

        log::info!("[board] TA7670X power-on sequence complete");
        Ok(())
    }

    fn build_modem_port(
        &self,
        peripherals: &mut Peripherals,
    ) -> Result<Arc<Mutex<dyn ModemPort + Send>>, BoardError> {
        let uart_config = UartConfig::new().baudrate(
            esp_idf_hal::units::Hertz(Config::UART_BAUD)
        );

        // Build UartDriver and extend its lifetime to 'static.
        //
        // SAFETY: `Peripherals::take()` is called once in main() and the resulting
        // struct lives for the entire program duration. The 'd lifetime in UartDriver<'d>
        // is a borrow-checker exclusivity mechanism to prevent two drivers on the same
        // UART port — not an indication the peripheral will be dropped. Since this
        // UartDriver is the sole user of UART1 for the program's lifetime, the transmute
        // to 'static is sound.
        let uart: UartDriver<'static> = unsafe {
            use esp_idf_hal::gpio::AnyIOPin;
            let tx = AnyIOPin::steal(Config::UART_TX);
            let rx = AnyIOPin::steal(Config::UART_RX);
            let driver = UartDriver::new(
                peripherals.uart1.reborrow(),
                tx, rx, Option::<AnyIOPin>::None, Option::<AnyIOPin>::None,
                &uart_config,
            ).map_err(|e| BoardError::Uart(e.to_string()))?;
            std::mem::transmute(driver)
        };

        let port = AtPort::new(uart);
        let mut modem = A76xxModem::new(port);
        modem
            .init(Config::MODEM_CELLULAR_DATA)
            .map_err(|e| BoardError::Uart(e.to_string()))?;

        Ok(Arc::new(Mutex::new(modem)))
    }
}

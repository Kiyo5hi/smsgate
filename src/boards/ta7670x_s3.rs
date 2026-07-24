//! LilyGo T-A7670X S3 Standard (H799) board implementation.

use super::{Board, BoardError, ModemVariant};
use crate::config::Config;
use crate::modem::{
    a76xx::{at::HardwareAtPort, A76xxModem},
    ModemPort,
};
use esp_idf_hal::{
    peripherals::Peripherals,
    uart::{config::Config as UartConfig, UartDriver},
};
use std::{
    sync::{Arc, Mutex},
    time::Duration,
};

// Fixed H799 control pins from LilyGo's T-A7670X-S3-Standard definition.
const MODEM_DTR_PIN: u8 = 7;
const POWER_SAVE_MODE_PIN: u8 = 42;

pub struct TA7670XS3;

impl Board for TA7670XS3 {
    fn modem_variant(&self) -> ModemVariant {
        ModemVariant::A76xx
    }
    fn uart_tx_pin(&self) -> u8 {
        Config::UART_TX
    }
    fn uart_rx_pin(&self) -> u8 {
        Config::UART_RX
    }
    fn uart_baud(&self) -> u32 {
        Config::UART_BAUD
    }
    fn pwrkey_pin(&self) -> u8 {
        Config::PWRKEY_PIN
    }
    fn reset_pin(&self) -> Option<u8> {
        None
    }

    fn init(&self, _peripherals: &mut Peripherals) -> Result<(), BoardError> {
        use esp_idf_hal::gpio::{AnyOutputPin, PinDriver};

        // Keep the modem awake and disable the board-level power-save switch.
        // H799 has no R2-style BOARD_POWERON or modem reset GPIO.
        let mut dtr = unsafe {
            PinDriver::output(AnyOutputPin::steal(MODEM_DTR_PIN))
                .map_err(|e| BoardError::Gpio(e.to_string()))?
        };
        dtr.set_low().map_err(|e| BoardError::Gpio(e.to_string()))?;

        let mut power_save = unsafe {
            PinDriver::output(AnyOutputPin::steal(POWER_SAVE_MODE_PIN))
                .map_err(|e| BoardError::Gpio(e.to_string()))?
        };
        power_save
            .set_high()
            .map_err(|e| BoardError::Gpio(e.to_string()))?;

        core::mem::forget(dtr);
        core::mem::forget(power_save);
        Ok(())
    }

    fn build_modem_port(
        &self,
        peripherals: &mut Peripherals,
    ) -> Result<Arc<Mutex<dyn ModemPort + Send>>, BoardError> {
        let uart_config = UartConfig::new().baudrate(esp_idf_hal::units::Hertz(Config::UART_BAUD));

        let uart: UartDriver<'static> = unsafe {
            use esp_idf_hal::gpio::AnyIOPin;
            let tx = AnyIOPin::steal(Config::UART_TX);
            let rx = AnyIOPin::steal(Config::UART_RX);
            let driver = UartDriver::new(
                peripherals.uart1.reborrow(),
                tx,
                rx,
                Option::<AnyIOPin>::None,
                Option::<AnyIOPin>::None,
                &uart_config,
            )
            .map_err(|e| BoardError::Uart(e.to_string()))?;
            std::mem::transmute(driver)
        };

        let mut port = HardwareAtPort::new(uart);

        // ESP-only resets leave the modem power domain untouched. Probe before
        // PWRKEY so a reflash cannot toggle an already-running modem off.
        let modem_already_on = port.send_at("").map(|r| r.ok).unwrap_or(false);
        if modem_already_on {
            log::info!("[board] modem already powered, skipping PWRKEY pulse");
        } else {
            use esp_idf_hal::gpio::{AnyOutputPin, PinDriver};
            let mut pwrkey = unsafe {
                PinDriver::output(AnyOutputPin::steal(Config::PWRKEY_PIN))
                    .map_err(|e| BoardError::Gpio(e.to_string()))?
            };
            pwrkey
                .set_low()
                .map_err(|e| BoardError::Gpio(e.to_string()))?;
            std::thread::sleep(Duration::from_millis(100));
            pwrkey
                .set_high()
                .map_err(|e| BoardError::Gpio(e.to_string()))?;
            std::thread::sleep(Duration::from_millis(1000));
            pwrkey
                .set_low()
                .map_err(|e| BoardError::Gpio(e.to_string()))?;
            log::info!("[board] modem silent, PWRKEY pulse complete");
        }

        let mut modem = A76xxModem::new(port);
        modem
            .init(
                Config::MODEM_CELLULAR_DATA,
                Config::MODEM_DISABLE_CELLULAR_DATA,
                Config::MODEM_SIM_PIN,
            )
            .map_err(|e| BoardError::Uart(e.to_string()))?;

        Ok(Arc::new(Mutex::new(modem)))
    }
}

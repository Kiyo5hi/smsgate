//! smsgate — Rust rewrite
//!
//! This is the composition root. Architecture mirrors the C++ version but uses
//! Rust's type system to eliminate whole categories of bugs:
//!   - No uint32_t millis() wrap issues (use esp_idf_hal::delay::FreeRtosDelay or
//!     std::time::Instant when available)
//!   - No DynamicJsonDocument sizing — serde handles allocation
//!   - No signed/unsigned cast UB in timer arithmetic
//!
//! TODO: implement each module.

fn main() {
    // It is necessary to call this function once. Otherwise some patches to the runtime
    // implemented by esp-idf-sys might not link properly. See https://github.com/esp-rs/esp-idf-template
    esp_idf_sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    log::info!("smsgate starting...");

    // TODO: initialise WiFi, modem, Telegram client, SMS handler, poller
    loop {
        // TODO: main event loop
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
}

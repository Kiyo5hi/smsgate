//! smsgate — library root.
//! Exposes all modules for use by the harness test crate.

#[cfg(feature = "esp32")]
pub mod boards;
pub mod bridge;
pub mod commands;
pub mod config;
pub mod im;
pub mod log_ring;
pub mod modem;
pub mod persist;
pub mod sms;
pub mod timer;

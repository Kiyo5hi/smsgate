//! Test harness — mock implementations of all I/O boundaries.
//!
//! Provides:
//! - `ScriptedModem` — programmable AT response script
//! - `RecordingMessenger` — captures sent messages; injects inbound
//! - `MemStore` (re-exported from smsgate)
//! - `Scenario` — declarative end-to-end test DSL

pub mod mocks;
pub mod scenario;

pub use mocks::{FailingMessenger, RecordingMessenger, ScriptedModem};
pub use smsgate::persist::mem::MemStore;
pub use scenario::Scenario;

/// Strip all ASCII whitespace from a PDU hex literal (for readable test constants).
pub fn pdu(hex: &str) -> String {
    hex.chars().filter(|c| !c.is_ascii_whitespace()).collect()
}

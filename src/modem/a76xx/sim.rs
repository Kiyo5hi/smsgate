//! SIM readiness and optional PIN unlock helpers.

use crate::modem::{AtTransport, ModemError};
use std::time::{Duration, Instant};

const SIM_READY_TIMEOUT: Duration = Duration::from_secs(10);
const SIM_READY_POLL: Duration = Duration::from_millis(500);

enum SimPinStatus {
    Ready,
    PinRequired,
    PukRequired,
    Other(String),
}

pub fn ensure_sim_unlocked<T: AtTransport + ?Sized>(
    modem: &mut T,
    sim_pin: &str,
) -> Result<(), ModemError> {
    let sim_pin = sim_pin.trim();
    validate_sim_pin(sim_pin)?;
    match query_sim_pin_status(modem)? {
        SimPinStatus::Ready => {
            log::info!("[sim] SIM ready");
            Ok(())
        }
        SimPinStatus::PinRequired => unlock_with_pin(modem, sim_pin),
        SimPinStatus::PukRequired => Err(ModemError::AtError(
            "SIM requires PUK; refusing PIN unlock".into(),
        )),
        SimPinStatus::Other(status) => Err(ModemError::AtError(format!(
            "unsupported SIM PIN status: {}",
            status
        ))),
    }
}

fn unlock_with_pin<T: AtTransport + ?Sized>(
    modem: &mut T,
    sim_pin: &str,
) -> Result<(), ModemError> {
    if sim_pin.is_empty() {
        return Err(ModemError::AtError(
            "SIM requires PIN but modem.sim_pin is empty".into(),
        ));
    }
    let response = modem.send_at(&format!("+CPIN=\"{}\"", sim_pin))?;
    if !response.ok {
        return Err(ModemError::AtError("SIM PIN unlock rejected".into()));
    }
    log::info!("[sim] SIM PIN accepted; waiting for READY");
    let deadline = Instant::now() + SIM_READY_TIMEOUT;
    loop {
        match query_sim_pin_status(modem)? {
            SimPinStatus::Ready => return Ok(()),
            SimPinStatus::PukRequired => {
                return Err(ModemError::AtError(
                    "SIM requires PUK after PIN unlock attempt".into(),
                ));
            }
            SimPinStatus::PinRequired | SimPinStatus::Other(_) => {}
        }
        if Instant::now() >= deadline {
            return Err(ModemError::Timeout);
        }
        std::thread::sleep(SIM_READY_POLL);
    }
}

fn validate_sim_pin(sim_pin: &str) -> Result<(), ModemError> {
    if sim_pin.is_empty()
        || ((4..=8).contains(&sim_pin.len()) && sim_pin.bytes().all(|byte| byte.is_ascii_digit()))
    {
        Ok(())
    } else {
        Err(ModemError::AtError("SIM PIN must be 4-8 digits".into()))
    }
}

fn query_sim_pin_status<T: AtTransport + ?Sized>(
    modem: &mut T,
) -> Result<SimPinStatus, ModemError> {
    let response = modem.send_at("+CPIN?")?;
    if !response.ok {
        return Err(ModemError::AtError(format!(
            "AT+CPIN? failed: {}",
            response.body.trim()
        )));
    }
    let status = response
        .body
        .lines()
        .find_map(|line| line.trim().strip_prefix("+CPIN:"))
        .map(str::trim)
        .unwrap_or_else(|| response.body.trim());
    Ok(match status {
        "READY" => SimPinStatus::Ready,
        "SIM PIN" => SimPinStatus::PinRequired,
        "SIM PUK" => SimPinStatus::PukRequired,
        other => SimPinStatus::Other(other.to_string()),
    })
}

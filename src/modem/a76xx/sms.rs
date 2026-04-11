//! PDU send/read/delete AT command flows for A76xx.

use crate::modem::ModemError;
use super::at::AtPort;
use std::time::Duration;

/// Send a PDU SMS via AT+CMGS.
/// Returns the message reference number (MR) on success.
pub fn send_pdu(port: &mut AtPort, hex: &str, tpdu_len: u8) -> Result<u8, ModemError> {
    // Issue AT+CMGS=<tpduLen>
    let cmd = format!("+CMGS={}", tpdu_len);
    port.write_raw(format!("AT{}\r", cmd).as_bytes())?;

    // Wait for '>' prompt (10 s)
    if !port.wait_for_prompt(b'>', Duration::from_secs(10)) {
        return Err(ModemError::Timeout);
    }

    // Send PDU hex followed by Ctrl-Z (0x1A)
    port.write_raw(hex.as_bytes())?;
    port.write_raw(&[0x1A])?;

    // Wait for +CMGS: or OK (60 s for network round-trip)
    let deadline = std::time::Instant::now() + Duration::from_secs(60);
    let mut lines: Vec<String> = Vec::new();
    loop {
        if std::time::Instant::now() > deadline {
            return Err(ModemError::Timeout);
        }
        // Kick the Task Watchdog Timer — SMS send can block > 30 s on congested networks.
        unsafe { esp_idf_sys::esp_task_wdt_reset(); }
        // Read using a generous timeout per line
        if let Some(line) = read_line_raw(port, Duration::from_secs(5)) {
            let line = line.trim().to_string();
            if line.is_empty() { continue; }
            if line == "OK" {
                break;
            }
            if line.starts_with("ERROR") || line.starts_with("+CMS ERROR") {
                return Err(ModemError::AtError(line));
            }
            lines.push(line);
        }
    }

    // Parse +CMGS: <mr>
    for line in &lines {
        if let Some(rest) = line.strip_prefix("+CMGS:") {
            let mr: u8 = rest.trim().parse().unwrap_or(0);
            return Ok(mr);
        }
    }

    // OK but no +CMGS: line — some firmware variants omit it.
    // Return 0 as a sentinel (caller treats 0 as "unknown MR").
    Ok(0)
}

/// Read an SMS PDU from a slot via AT+CMGR.
/// Returns (hex_pdu, tpdu_len_hint) or None.
pub fn read_pdu(port: &mut AtPort, slot: u16) -> Option<String> {
    let cmd = format!("+CMGR={}", slot);
    let r = port.send_at(&cmd).ok()?;
    if !r.ok { return None; }
    // Response: +CMGR: <stat>,,<len>\r\n<hex>\r\nOK
    // The PDU hex is the line after +CMGR:
    let lines: Vec<&str> = r.body.lines().collect();
    lines.into_iter()
        .find(|l| !l.starts_with("+CMGR:") && !l.is_empty())
        .map(|s| s.trim().to_string())
}

/// Delete SMS at a given slot.
pub fn delete_pdu(port: &mut AtPort, slot: u16) {
    let cmd = format!("+CMGD={}", slot);
    let _ = port.send_at(&cmd);
}

/// List all SMS slots via AT+CMGL=4 (all messages, PDU mode).
/// Returns list of (slot, hex_pdu).
pub fn list_all_pdus(port: &mut AtPort) -> Vec<(u16, String)> {
    let r = match port.send_at("+CMGL=4") {
        Ok(r) => r,
        Err(_) => return vec![],
    };
    if !r.ok { return vec![]; }

    let mut result = Vec::new();
    let mut lines = r.body.lines().peekable();
    while let Some(line) = lines.next() {
        if let Some(rest) = line.strip_prefix("+CMGL: ") {
            let slot: u16 = rest.split(',').next()
                .and_then(|s| s.trim().parse().ok())
                .unwrap_or(0);
            if let Some(pdu_line) = lines.next() {
                let hex = pdu_line.trim().to_string();
                if !hex.is_empty() {
                    result.push((slot, hex));
                }
            }
        }
    }
    result
}

fn read_line_raw(port: &mut AtPort, timeout: Duration) -> Option<String> {
    // We reuse poll_urc which has a short timeout — call in loop until we get a real line
    let deadline = std::time::Instant::now() + timeout;
    loop {
        if std::time::Instant::now() > deadline { return None; }
        if let Some(l) = port.poll_urc() {
            return Some(l);
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

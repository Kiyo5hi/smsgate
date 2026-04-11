//! URC (Unsolicited Result Code) classifier and parser.

/// Returns true iff `line` looks like a URC (not a command response).
pub fn is_urc(line: &str) -> bool {
    // Known URC prefixes from A76xx
    matches!(
        true,
        _ if line.starts_with("+CMTI:") ||
             line.starts_with("+CMT:")  ||
             line.starts_with("+CDSI:") ||
             line.starts_with("+CDS:")  ||
             line.starts_with("+CLIP:") ||
             line.starts_with("RING")   ||
             line.starts_with("+CREG:") ||
             line.starts_with("+CGREG:")||
             line.starts_with("+CEREG:")||
             line.starts_with("NO CARRIER") ||
             line.starts_with("+CUSD:")
    )
}

/// Parsed URC discriminant.
#[derive(Debug)]
pub enum Urc {
    /// New SMS stored: (memory, index).
    NewSms { index: u16 },
    /// Direct SMS delivery (CMT mode): (PDU hex, TPDU len) — followed by a second line.
    SmsDelivery,
    /// Incoming call (RING without CLIP yet).
    Ring,
    /// Caller ID: (phone, type).
    Clip(String),
    /// Registration status change.
    Creg,
    /// Status report available.
    StatusReport,
    /// Other / unrecognised.
    Other(String),
}

/// Parse a URC line to a discriminant.
pub fn parse_urc(line: &str) -> Urc {
    if let Some(rest) = line.strip_prefix("+CMTI: ") {
        // +CMTI: "SM",3  or  +CMTI: "ME",3
        let idx: u16 = rest.split(',').nth(1)
            .and_then(|s| s.trim().parse().ok())
            .unwrap_or(0);
        return Urc::NewSms { index: idx };
    }
    if line.starts_with("+CMT:") {
        return Urc::SmsDelivery;
    }
    if line == "RING" || line.starts_with("RING") {
        return Urc::Ring;
    }
    if let Some(rest) = line.strip_prefix("+CLIP: ") {
        let number = crate::sms::codec::parse_clip_line(&format!("+CLIP: {}", rest))
            .unwrap_or_default();
        return Urc::Clip(number);
    }
    if line.starts_with("+CDS:") || line.starts_with("+CDSI:") {
        return Urc::StatusReport;
    }
    if line.starts_with("+CREG:") || line.starts_with("+CGREG:") || line.starts_with("+CEREG:") {
        return Urc::Creg;
    }
    Urc::Other(line.to_string())
}

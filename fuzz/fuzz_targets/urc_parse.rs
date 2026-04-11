#![no_main]

use libfuzzer_sys::fuzz_target;
use smsgate::sms::codec::{parse_clip_line, parse_sms_pdu};

fuzz_target!(|data: &[u8]| {
    if let Ok(s) = std::str::from_utf8(data) {
        let _ = parse_clip_line(s);
    }
    // Also fuzz PDU decode with raw bytes converted to hex
    let hex: String = data.iter().map(|b| format!("{:02X}", b)).collect();
    let _ = parse_sms_pdu(&hex);
});

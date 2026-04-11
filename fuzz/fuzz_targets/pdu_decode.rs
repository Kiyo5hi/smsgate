#![no_main]

use libfuzzer_sys::fuzz_target;
use smsgate::sms::codec::{parse_sms_pdu, parse_status_report};

fuzz_target!(|data: &[u8]| {
    let hex: String = data.iter().map(|b| format!("{:02X}", b)).collect();
    let _ = parse_sms_pdu(&hex);
    let _ = parse_status_report(&hex);
});

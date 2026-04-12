//! Compile-time configuration injected by build.rs from config.toml.
//! This module contains *only* env!() references — no hardcoded values.

pub struct Config;

impl Config {
    pub const WIFI_SSID: &'static str = env!("CFG_WIFI_SSID");
    pub const WIFI_PASSWORD: &'static str = env!("CFG_WIFI_PASSWORD");
    pub const IM_BACKEND: &'static str = env!("CFG_IM_BACKEND");
    pub const BOT_TOKEN: &'static str = env!("CFG_IM_BOT_TOKEN");
    pub const CHAT_ID: i64 = {
        // parse at compile time; default 0 if empty/missing
        let s = env!("CFG_IM_CHAT_ID");
        parse_i64_const(s)
    };
    pub const UART_TX: u8 = parse_u8_const(env!("CFG_MODEM_UART_TX"));
    pub const UART_RX: u8 = parse_u8_const(env!("CFG_MODEM_UART_RX"));
    pub const UART_BAUD: u32 = parse_u32_const(env!("CFG_MODEM_UART_BAUD"));
    pub const PWRKEY_PIN: u8 = parse_u8_const(env!("CFG_MODEM_PWRKEY"));
    pub const MAX_FAILURES: u8 = parse_u8_const(env!("CFG_BRIDGE_MAX_FAILURES"));
    pub const POLL_INTERVAL_MS: u32 = parse_u32_const(env!("CFG_BRIDGE_POLL_INTERVAL_MS"));
    pub const WATCHDOG_TIMEOUT_SEC: u32 = parse_u32_const(env!("CFG_BRIDGE_WATCHDOG_SEC"));
    pub const GIT_COMMIT: &'static str = env!("CFG_GIT_COMMIT");
}

const fn parse_u8_const(s: &str) -> u8 {
    parse_u64_const(s) as u8
}

const fn parse_u32_const(s: &str) -> u32 {
    parse_u64_const(s) as u32
}

const fn parse_i64_const(s: &str) -> i64 {
    let bytes = s.as_bytes();
    if bytes.is_empty() {
        return 0;
    }
    let (neg, start) = if bytes[0] == b'-' { (true, 1) } else { (false, 0) };
    let mut i = start;
    let mut acc: i64 = 0;
    while i < bytes.len() {
        let d = bytes[i];
        if d >= b'0' && d <= b'9' {
            acc = acc * 10 + (d - b'0') as i64;
        }
        i += 1;
    }
    if neg { -acc } else { acc }
}

const fn parse_u64_const(s: &str) -> u64 {
    let bytes = s.as_bytes();
    let mut i = 0;
    let mut acc: u64 = 0;
    while i < bytes.len() {
        let d = bytes[i];
        if d >= b'0' && d <= b'9' {
            acc = acc * 10 + (d - b'0') as u64;
        }
        i += 1;
    }
    acc
}

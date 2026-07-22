use smsgate::log_clock::{parse_cclk_time, LogClock};

#[test]
fn parses_simcom_cclk_timezone_quarters() {
    let time = parse_cclk_time("+CCLK: \"26/07/21,12:34:56-28\"").unwrap();
    assert_eq!(time.offset_minutes, -420);
    assert_eq!(time.format(), "2026-07-21 12:34:56-07:00");
}

#[test]
fn rejects_stale_or_invalid_network_clock() {
    assert!(parse_cclk_time("+CCLK: \"20/01/01,00:00:00+00\"").is_none());
    assert!(parse_cclk_time("+CCLK: \"70/01/01,00:00:00+00\"").is_none());
    assert!(parse_cclk_time("+CCLK: \"26/13/01,00:00:00+00\"").is_none());
}

#[test]
fn synced_clock_advances_across_u32_wrap() {
    let mut clock = LogClock::new();
    let time = parse_cclk_time("+CCLK: \"26/07/21,23:59:58+00\"").unwrap();
    clock.sync_from_network(u32::MAX - 999, time);
    assert_eq!(clock.timestamp(1_000), "2026-07-22 00:00:00+00:00");
}

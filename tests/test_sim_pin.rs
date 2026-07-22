use smsgate::modem::a76xx::sim::ensure_sim_unlocked;
use smsgate::testing::mocks::ScriptedModem;

#[test]
fn ready_sim_does_not_send_pin() {
    let mut modem = ScriptedModem::new().expect("+CPIN?", "+CPIN: READY", true);
    ensure_sim_unlocked(&mut modem, "").unwrap();
    modem.check_consumed();
}

#[test]
fn locked_sim_sends_configured_pin() {
    let mut modem = ScriptedModem::new()
        .expect("+CPIN?", "+CPIN: SIM PIN", true)
        .expect("+CPIN=\"1234\"", "", true)
        .expect("+CPIN?", "+CPIN: READY", true);
    ensure_sim_unlocked(&mut modem, "1234").unwrap();
    modem.check_consumed();
}

#[test]
fn locked_sim_without_pin_fails() {
    let mut modem = ScriptedModem::new().expect("+CPIN?", "+CPIN: SIM PIN", true);
    assert!(ensure_sim_unlocked(&mut modem, "")
        .unwrap_err()
        .to_string()
        .contains("requires PIN"));
    modem.check_consumed();
}

#[test]
fn invalid_pin_is_rejected_without_at_command() {
    let mut modem = ScriptedModem::new();
    assert!(ensure_sim_unlocked(&mut modem, "12ab")
        .unwrap_err()
        .to_string()
        .contains("4-8 digits"));
    modem.check_consumed();
}

#[test]
fn puk_locked_sim_refuses_pin() {
    let mut modem = ScriptedModem::new().expect("+CPIN?", "+CPIN: SIM PUK", true);
    assert!(ensure_sim_unlocked(&mut modem, "1234")
        .unwrap_err()
        .to_string()
        .contains("requires PUK"));
    modem.check_consumed();
}

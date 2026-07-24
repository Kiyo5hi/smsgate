use smsgate::modem::{creg_registered, ModemPort};
use smsgate::testing::mocks::ScriptedModem;

#[test]
fn registration_parser_accepts_lte_roaming_and_sms_only_states() {
    assert!(creg_registered("+CEREG: 0,5"));
    assert!(creg_registered("+CGREG: 0,6"));
    assert!(creg_registered("+CEREG: 0,7"));
    assert!(!creg_registered("+CEREG: 0,2"));
}

#[test]
fn status_uses_lte_registration_on_lte_only_networks() {
    let mut modem = ScriptedModem::new()
        .expect("+CSQ", "+CSQ: 20,0", true)
        .expect("+COPS?", "+COPS: 0,0,\"310410\",7", true)
        .expect("+CEREG?", "+CEREG: 0,5", true);

    let status = modem.update_status();

    assert!(status.registered);
    assert_eq!(status.operator, "310410");
    modem.check_consumed();
}

#[test]
fn status_falls_back_to_circuit_switched_registration() {
    let mut modem = ScriptedModem::new()
        .expect("+CSQ", "+CSQ: 15,0", true)
        .expect("+COPS?", "+COPS: 0,0,\"Legacy\",0", true)
        .expect("+CEREG?", "+CEREG: 0,0", true)
        .expect("+CREG?", "+CREG: 0,1", true);

    let status = modem.update_status();

    assert!(status.registered);
    modem.check_consumed();
}

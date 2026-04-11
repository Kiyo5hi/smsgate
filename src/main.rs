//! smsgate — composition root.

#[cfg(feature = "esp32")]
use smsgate::{
    boards::{ta7670x::TA7670X, Board},
    bridge::{
        call_handler::CallHandler,
        forwarder::forward_sms,
        poller::poll_and_dispatch,
        reply_router::ReplyRouter,
    },
    commands::{
        builtin::*,
        CommandRegistry,
    },
    config::Config,
    im::{Messenger, telegram::{http::TelegramHttpClient, TelegramMessenger}},
    log_ring::LogRing,
    modem::urc::{parse_urc, Urc},
    persist::nvs::NvsStore,
    sms::{
        codec::parse_sms_pdu,
        concat::ConcatReassembler,
        sender::SmsSender,
        SmsMessage,
    },
    timer::elapsed_since,
};

#[cfg(not(feature = "esp32"))]
fn main() {
    panic!("This binary requires the esp32 feature");
}

#[cfg(feature = "esp32")]
fn main() {
    esp_idf_sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    log::info!("smsgate starting…");

    // ---- Board init ----
    let mut peripherals = esp_idf_hal::peripherals::Peripherals::take().unwrap();
    let board = TA7670X;
    board.init(&mut peripherals).expect("board init failed");
    let mut modem = board.build_modem_port(&mut peripherals).expect("modem init failed");

    // ---- NVS store (RFC-0025: fall back to MemStore on NVS failure) ----
    let nvs_partition = esp_idf_svc::nvs::EspDefaultNvsPartition::take().unwrap();
    let nvs_failed: bool;
    let mut store: Box<dyn smsgate::persist::Store> = match NvsStore::new(nvs_partition) {
        Ok(nvs) => {
            nvs_failed = false;
            Box::new(nvs)
        }
        Err(e) => {
            nvs_failed = true;
            log::error!("[main] NVS init failed: {} — using volatile MemStore", e);
            Box::new(smsgate::persist::mem::MemStore::default())
        }
    };

    // ---- WiFi ----
    let sysloop = esp_idf_svc::eventloop::EspSystemEventLoop::take().unwrap();
    // SAFETY: EspWifi borrows the modem peripheral whose lifetime is tied to
    // `peripherals`, which lives for the duration of main(). Transmuting to
    // 'static is sound because the wifi driver is kept alive until main exits.
    let wifi_inner: esp_idf_svc::wifi::EspWifi<'static> = unsafe {
        std::mem::transmute(
            esp_idf_svc::wifi::EspWifi::new(
                peripherals.modem, sysloop.clone(), None
            ).expect("WiFi init failed")
        )
    };
    let mut wifi = esp_idf_svc::wifi::BlockingWifi::wrap(wifi_inner, sysloop.clone())
        .expect("WiFi wrap failed");
    setup_wifi(&mut wifi).expect("WiFi connect failed");
    let _wifi = wifi; // keep WiFi driver alive

    // ---- IM (Telegram) ----
    let http = TelegramHttpClient::new(None).expect("TLS init failed");
    let mut messenger = TelegramMessenger::new(http);

    // ---- Subsystems ----
    let mut sender = SmsSender::new();
    let mut router = ReplyRouter::new();
    router.load(&*store);
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut call_handler = CallHandler::new();
    let mut modem_status = smsgate::modem::ModemStatus::default();

    // ---- Command registry ----
    let mut registry = CommandRegistry::new();
    // Build help text first (needs to know all commands — use placeholder, updated below)
    registry.register(Box::new(HelpCommand { help_text: String::new() }));
    registry.register(Box::new(StatusCommand));
    registry.register(Box::new(SendCommand));
    registry.register(Box::new(LogCommand));
    registry.register(Box::new(QueueCommand));
    registry.register(Box::new(BlockCommand));
    registry.register(Box::new(UnblockCommand));
    registry.register(Box::new(PauseCommand));
    registry.register(Box::new(ResumeCommand));
    registry.register(Box::new(RestartCommand));

    // Re-create with correct help text (registry is rebuilt after all commands registered)
    let help_text = registry.help_text();
    let mut registry = CommandRegistry::new();
    registry.register(Box::new(HelpCommand { help_text }));
    registry.register(Box::new(StatusCommand));
    registry.register(Box::new(SendCommand));
    registry.register(Box::new(LogCommand));
    registry.register(Box::new(QueueCommand));
    registry.register(Box::new(BlockCommand));
    registry.register(Box::new(UnblockCommand));
    registry.register(Box::new(PauseCommand));
    registry.register(Box::new(ResumeCommand));
    registry.register(Box::new(RestartCommand));

    // Register bot commands with Telegram
    if let Err(e) = messenger.register_commands(&registry.command_list()) {
        log::warn!("[main] register_commands failed: {} — continuing", e);
    }

    // RFC-0025: alert if NVS failed (now that we have a messenger)
    if nvs_failed {
        let _ = messenger.send_message(
            "⚠️ NVS init failed — running without persistence. Block list and cursor will reset on reboot."
        );
    }

    // ---- Sweep existing SMS from SIM on boot ----
    sweep_existing_sms(&mut *modem, &mut router, &mut log, &mut concat,
                        &mut messenger, &mut *store);

    log::info!("smsgate ready");
    let _ = messenger.send_message("✅ smsgate started");

    // Subscribe main task to the Task WDT (RFC-0001 §4.4: 120s timeout).
    // The WDT fires if esp_task_wdt_reset() is not called within the timeout.
    unsafe { esp_idf_sys::esp_task_wdt_add(std::ptr::null_mut()); }

    // ---- Main loop ----
    let boot_ms = now_ms();
    let mut consecutive_failures: u8 = 0;
    let mut last_status_update = now_ms();

    loop {
        let now = now_ms();
        let uptime_ms = elapsed_since(boot_ms, now);

        // Kick the hardware watchdog (RFC-0001 §4.4)
        unsafe { esp_idf_sys::esp_task_wdt_reset(); }

        // Update modem status every 30 s
        if elapsed_since(last_status_update, now) > 30_000 {
            if let Ok(s) = a76xx_update_status(&mut *modem) {
                modem_status = s;
            }
            last_status_update = now;

            // Low-heap alert
            check_low_heap(&mut messenger);
        }

        // Poll URCs (non-blocking)
        while let Some(urc) = modem.poll_urc() {
            match parse_urc(&urc) {
                Urc::NewSms { index } => {
                    handle_new_sms(index, &mut *modem, &mut router, &mut log,
                                   &mut concat, &mut messenger, &mut *store);
                }
                Urc::SmsDelivery => {
                    // Direct CMT delivery — the next URC line is the PDU
                    // (handled in the next poll_urc iteration via modem driver)
                }
                _ => {
                    call_handler.handle_urc(&urc, &mut *modem, &mut messenger, &mut sender);
                }
            }
        }
        call_handler.tick(&mut *modem, &mut messenger, &mut sender);

        // Poll IM messages and dispatch commands
        let poll_result = poll_and_dispatch(
            &mut messenger, &mut sender, &router, &registry,
            &mut *store, &log, &modem_status, uptime_ms,
            (Config::POLL_INTERVAL_MS / 1000).max(1),
        );
        match poll_result {
            Ok(restart) => {
                consecutive_failures = 0;
                if restart {
                    log::info!("[main] restart requested via /restart command");
                    let _ = messenger.send_message("♻️ Rebooting now…");
                    esp_idf_hal::reset::restart();
                }
            }
            Err(e) => {
                consecutive_failures += 1;
                log::error!("[main] poll failed ({}): {}", consecutive_failures, e);
                if consecutive_failures >= Config::MAX_FAILURES {
                    log::error!("[main] max failures reached — rebooting");
                    esp_idf_hal::reset::restart();
                }
                std::thread::sleep(std::time::Duration::from_secs(5));
                continue;
            }
        }

        // Drain outbound SMS queue
        sender.drain_once(&mut *modem);
    }
}

#[cfg(feature = "esp32")]
fn now_ms() -> u32 {
    (esp_idf_svc::systime::EspSystemTime.now().as_millis() & 0xFFFF_FFFF) as u32
}

#[cfg(feature = "esp32")]
fn handle_new_sms(
    index: u16,
    modem: &mut dyn smsgate::modem::ModemPort,
    router: &mut ReplyRouter,
    log: &mut LogRing,
    concat: &mut ConcatReassembler,
    messenger: &mut dyn smsgate::im::Messenger,
    store: &mut dyn smsgate::persist::Store,
) {
    let r = modem.send_at(&format!("+CMGR={}", index));
    let pdu_hex = match r {
        Ok(resp) if resp.ok => {
            resp.body.lines()
                .find(|l| !l.starts_with("+CMGR:") && !l.is_empty())
                .map(|s| s.trim().to_string())
        }
        _ => None,
    };

    let Some(hex) = pdu_hex else {
        log::warn!("[main] could not read SMS at slot {}", index);
        return;
    };

    // Delete immediately
    let _ = modem.send_at(&format!("+CMGD={}", index));

    process_pdu_hex(&hex, index, router, log, concat, messenger, store);
}

#[cfg(feature = "esp32")]
fn process_pdu_hex(
    hex: &str,
    slot: u16,
    router: &mut ReplyRouter,
    log: &mut LogRing,
    concat: &mut ConcatReassembler,
    messenger: &mut dyn smsgate::im::Messenger,
    store: &mut dyn smsgate::persist::Store,
) {
    let pdu = match parse_sms_pdu(hex) {
        Ok(p) => p,
        Err(e) => { log::error!("[main] PDU parse error: {}", e); return; }
    };

    let sms = if pdu.is_concatenated {
        match concat.feed(&pdu) {
            Some(complete) => SmsMessage {
                sender: complete.sender,
                body: complete.content,
                timestamp: complete.timestamp,
                slot,
            },
            None => return, // still waiting for more parts
        }
    } else {
        SmsMessage {
            sender: pdu.sender,
            body: pdu.content,
            timestamp: pdu.timestamp,
            slot,
        }
    };

    forward_sms(&sms, messenger, router, log, store);
}

#[cfg(feature = "esp32")]
fn sweep_existing_sms(
    modem: &mut dyn smsgate::modem::ModemPort,
    router: &mut ReplyRouter,
    log: &mut LogRing,
    concat: &mut ConcatReassembler,
    messenger: &mut dyn smsgate::im::Messenger,
    store: &mut dyn smsgate::persist::Store,
) {
    log::info!("[main] sweeping existing SMS…");
    let r = modem.send_at("+CMGL=4");
    let Ok(resp) = r else { return; };
    if !resp.ok { return; }

    let body = resp.body.clone();
    let mut lines = body.lines().peekable();
    while let Some(line) = lines.next() {
        if let Some(rest) = line.strip_prefix("+CMGL: ") {
            let slot: u16 = rest.split(',').next()
                .and_then(|s| s.trim().parse().ok()).unwrap_or(0);
            if let Some(hex) = lines.next() {
                let hex = hex.trim();
                let _ = modem.send_at(&format!("+CMGD={}", slot));
                process_pdu_hex(hex, slot, router, log, concat, messenger, store);
            }
        }
    }
}

#[cfg(feature = "esp32")]
fn a76xx_update_status(modem: &mut dyn smsgate::modem::ModemPort)
    -> Result<smsgate::modem::ModemStatus, smsgate::modem::ModemError>
{
    let mut s = smsgate::modem::ModemStatus::default();
    if let Ok(r) = modem.send_at("+CSQ") {
        if let Some(v) = r.body.strip_prefix("+CSQ: ") {
            s.csq = v.split(',').next().and_then(|x| x.trim().parse().ok()).unwrap_or(99);
        }
    }
    if let Ok(r) = modem.send_at("+COPS?") {
        if let Some(start) = r.body.find('"') {
            if let Some(end) = r.body[start + 1..].find('"') {
                s.operator = r.body[start + 1..start + 1 + end].to_string();
            }
        }
    }
    if let Ok(r) = modem.send_at("+CREG?") {
        s.registered = r.body.contains(",1") || r.body.contains(",5");
    }
    Ok(s)
}

#[cfg(feature = "esp32")]
fn check_low_heap(messenger: &mut dyn smsgate::im::Messenger) {
    let free = unsafe { esp_idf_sys::esp_get_free_heap_size() };
    if free < 20 * 1024 {
        log::warn!("[main] low heap: {} bytes", free);
        let _ = messenger.send_message(&format!("⚠️ Low heap: {} bytes", free));
    }
}

#[cfg(feature = "esp32")]
fn setup_wifi(
    wifi: &mut esp_idf_svc::wifi::BlockingWifi<esp_idf_svc::wifi::EspWifi<'static>>,
) -> anyhow::Result<()> {
    use esp_idf_svc::wifi::{AuthMethod, ClientConfiguration, Configuration};
    use smsgate::config::Config;

    let config = Configuration::Client(ClientConfiguration {
        ssid: Config::WIFI_SSID.try_into().map_err(|_| anyhow::anyhow!("SSID too long"))?,
        password: Config::WIFI_PASSWORD.try_into().map_err(|_| anyhow::anyhow!("Password too long"))?,
        auth_method: AuthMethod::WPA2Personal,
        ..Default::default()
    });
    wifi.set_configuration(&config)?;
    wifi.start()?;
    wifi.connect()?;
    wifi.wait_netif_up()?;
    log::info!("[wifi] connected, IP assigned");
    Ok(())
}


//! smsgate — composition root.
//!
//! `anyhow` is intentionally limited to this file: it handles the startup
//! sequence where ergonomic error propagation matters and heap allocation is
//! acceptable. All inner modules use concrete `thiserror`-derived error types.

#[cfg(feature = "esp32")]
use smsgate::{
    boards::{ta7670x::TA7670X, Board},
    bridge::{
        call_handler::CallHandler,
        poller::poll_and_dispatch,
        reply_router::ReplyRouter,
        sms_handler::{handle_new_sms, process_pdu_hex, sweep_one_storage},
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
    sms::concat::ConcatReassembler,
    sms::sender::{DrainOutcome, SmsSender},
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
        let _ = messenger.send_message(smsgate::i18n::nvs_fail());
    }

    // ---- Sweep existing SMS from SIM on boot ----
    // Sweep both SM (SIM) and ME (device flash) in case SMS arrived before
    // CPMS was explicitly configured. Normal operation after init stores in ME.
    for mem in &["SM", "ME"] {
        let _ = modem.send_at(&format!("+CPMS=\"{}\",\"{}\",\"{}\"", mem, mem, mem));
        log::info!("[main] sweeping {} storage…", mem);
        sweep_one_storage(mem, &mut *modem, &mut router, &mut log, &mut concat,
                          &mut messenger, &mut *store);
    }

    log::info!("smsgate ready");
    let _ = messenger.send_message(smsgate::i18n::started());

    // Subscribe main task to the Task WDT (RFC-0001 §4.4: 120s timeout).
    // The WDT fires if esp_task_wdt_reset() is not called within the timeout.
    unsafe { esp_idf_sys::esp_task_wdt_add(std::ptr::null_mut()); }

    // ---- Telegram polling thread ----
    // Runs getUpdates (long-poll) independently so the main loop is never blocked
    // waiting for the network. The channel delivers batches of inbound messages.
    let initial_cursor = smsgate::persist::load_i64(&*store, smsgate::persist::keys::IM_CURSOR)
        .unwrap_or(0);
    let (tg_tx, tg_rx) =
        std::sync::mpsc::channel::<Vec<smsgate::im::InboundMessage>>();
    std::thread::Builder::new()
        .name("tg-poll".into())
        .stack_size(16 * 1024)
        .spawn(move || {
            let http = TelegramHttpClient::new(None).expect("tg-poll: TLS init failed");
            let mut poll_messenger = TelegramMessenger::new(http);
            let mut cursor = initial_cursor;
            loop {
                match poll_messenger.poll(cursor, (Config::POLL_INTERVAL_MS / 1000).max(1)) {
                    Ok(msgs) if !msgs.is_empty() => {
                        cursor = msgs.iter().map(|m| m.cursor).max().unwrap_or(cursor);
                        if tg_tx.send(msgs).is_err() {
                            break; // main thread gone — exit cleanly
                        }
                    }
                    Ok(_) => {} // no new messages, poll again immediately
                    Err(e) => {
                        log::error!("[tg-poll] error: {}", e);
                        std::thread::sleep(std::time::Duration::from_secs(5));
                    }
                }
            }
        })
        .expect("failed to spawn tg-poll thread");

    // ---- Main loop ----
    let boot_ms = now_ms();
    let mut consecutive_failures: u8 = 0;
    let mut last_status_update = now_ms();
    let mut pause_until: Option<std::time::Instant> = None;
    let mut wifi_info = fmt_wifi(None);
    // +CMT direct delivery is two lines: header then raw PDU hex.
    // This flag is set when the header arrives so the next poll_urc() line
    // is treated as the PDU rather than a new URC.
    let mut cmt_pdu_pending = false;

    loop {
        let now = now_ms();
        let uptime_ms = elapsed_since(boot_ms, now);

        // Kick the hardware watchdog (RFC-0001 §4.4)
        unsafe { esp_idf_sys::esp_task_wdt_reset(); }

        // Auto-resume after timed /pause
        if let Some(until) = pause_until {
            if std::time::Instant::now() >= until {
                pause_until = None;
                let _ = smsgate::persist::save_bool(&mut *store, smsgate::persist::keys::FWD_ENABLED, true);
                let _ = messenger.send_message(smsgate::i18n::resume_ok());
                log::info!("[main] pause expired — forwarding re-enabled");
            }
        }

        // Update modem status every 30 s.
        // Skip while cmt_pdu_pending: send_at() drains the UART buffer and would
        // consume the PDU line that belongs to the pending +CMT delivery.
        if elapsed_since(last_status_update, now) > 30_000 && !cmt_pdu_pending {
            if let Ok(s) = a76xx_update_status(&mut *modem) {
                modem_status = s;
            }
            last_status_update = now;

            // Refresh WiFi RSSI
            let rssi = unsafe {
                let mut ap: esp_idf_sys::wifi_ap_record_t = std::mem::zeroed();
                if esp_idf_sys::esp_wifi_sta_get_ap_info(&mut ap) == esp_idf_sys::ESP_OK {
                    Some(ap.rssi as i32)
                } else {
                    None
                }
            };
            wifi_info = fmt_wifi(rssi);

            // Low-heap alert
            check_low_heap(&mut messenger);
        }

        // Poll URCs (non-blocking)
        while let Some(urc) = modem.poll_urc() {
            log::info!("[main] URC: {:?}", urc);

            // +CMT two-line protocol: header sets the flag, next line is the PDU.
            // Direct delivery has no modem slot — nothing to delete afterwards.
            if cmt_pdu_pending {
                cmt_pdu_pending = false;
                process_pdu_hex(urc.trim(), 0, &mut router, &mut log,
                                &mut concat, &mut messenger, &mut *store);
                continue;
            }

            match parse_urc(&urc) {
                Urc::NewSms { mem, index } => {
                    handle_new_sms(&mem, index, &mut *modem, &mut router, &mut log,
                                   &mut concat, &mut messenger, &mut *store);
                }
                Urc::SmsDelivery => {
                    cmt_pdu_pending = true; // next poll_urc() line is the raw PDU
                }
                _ => {
                    call_handler.handle_urc(&urc, &mut *modem, &mut messenger, &mut sender);
                }
            }
        }
        call_handler.tick(&mut *modem, &mut messenger, &mut sender);

        // Collect any Telegram messages delivered by the polling thread
        let tg_messages: Vec<smsgate::im::InboundMessage> = {
            let mut batch = Vec::new();
            loop {
                match tg_rx.try_recv() {
                    Ok(msgs) => batch.extend(msgs),
                    Err(std::sync::mpsc::TryRecvError::Empty) => break,
                    Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                        log::error!("[main] tg-poll thread died — rebooting");
                        esp_idf_hal::reset::restart();
                    }
                }
            }
            batch
        };

        // Dispatch commands and replies, update cursor in NVS
        if !tg_messages.is_empty() {
            if let Some(new_cursor) = tg_messages.iter().map(|m| m.cursor).max() {
                let _ = smsgate::persist::save_i64(
                    &mut *store, smsgate::persist::keys::IM_CURSOR, new_cursor,
                );
            }
            let free_heap = unsafe { esp_idf_sys::esp_get_free_heap_size() };
            match poll_and_dispatch(
                &tg_messages, &mut messenger, &mut sender, &router, &registry,
                &mut *store, &log, &modem_status, uptime_ms, free_heap, &wifi_info,
            ) {
                Ok((restart, maybe_pause)) => {
                    consecutive_failures = 0;
                    if let Some(mins) = maybe_pause {
                        pause_until = Some(std::time::Instant::now()
                            + std::time::Duration::from_secs(mins as u64 * 60));
                        log::info!("[main] pause timer set for {} min", mins);
                    }
                    if restart {
                        log::info!("[main] restart requested via /restart command");
                        let _ = messenger.send_message(smsgate::i18n::rebooting());
                        esp_idf_hal::reset::restart();
                    }
                }
                Err(e) => {
                    consecutive_failures += 1;
                    log::error!("[main] send failed ({}): {}", consecutive_failures, e);
                    if consecutive_failures >= Config::MAX_FAILURES {
                        log::error!("[main] max failures reached — rebooting");
                        esp_idf_hal::reset::restart();
                    }
                }
            }
        }

        // Drain outbound SMS queue
        match sender.drain_once(&mut *modem) {
            DrainOutcome::Sent { phone } => {
                let _ = messenger.send_message(&smsgate::i18n::sms_sent_ok(&phone));
            }
            DrainOutcome::Dropped { phone } => {
                let _ = messenger.send_message(&smsgate::i18n::sms_failed(&phone));
            }
            _ => {}
        }

        // Yield to other tasks; keeps URC latency under 100 ms without busy-looping.
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
}

#[cfg(feature = "esp32")]
fn fmt_wifi(rssi: Option<i32>) -> String {
    match rssi {
        Some(r) => format!("{} ({} dBm)", smsgate::config::Config::WIFI_SSID, r),
        None    => format!("{} (--)", smsgate::config::Config::WIFI_SSID),
    }
}

#[cfg(feature = "esp32")]
fn now_ms() -> u32 {
    (esp_idf_svc::systime::EspSystemTime.now().as_millis() & 0xFFFF_FFFF) as u32
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
        let _ = messenger.send_message(&smsgate::i18n::low_heap(free));
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
    log::info!("[wifi] connected");
    Ok(())
}

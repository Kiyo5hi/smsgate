//! IM message poll loop and command dispatcher.

use crate::bridge::reply_router::ReplyRouter;
use crate::commands::{
    builtin::log_cmd, CommandContext, CommandRegistry, BLOCK_SENTINEL, PAUSE_SENTINEL,
    RESTART_SENTINEL, RESUME_SENTINEL, SEND_SENTINEL, UNBLOCK_SENTINEL, UPDATE_CONFIRM_SENTINEL,
    UPDATE_SENTINEL,
};
use crate::im::{InboundMessage, MessageSink, MessengerError};
use crate::log_ring::LogRing;
use crate::modem::ModemStatus;
use crate::persist::{keys, save_bool, Store};
use crate::sms::sender::{CmdSendResult, SmsSender};

/// OTA action requested by a command.
#[derive(Debug, Clone, PartialEq)]
pub enum OtaAction {
    None,
    Update,
    Confirm,
}

/// Process a batch of inbound IM messages: dispatch commands and route replies to SMS.
/// Returns `(restart_requested, pause_mins, ota_action)`.
///
/// Polling is handled by a dedicated background thread; this function only processes
/// messages that have already been received. Cursor persistence is the caller's responsibility.
#[allow(clippy::too_many_arguments)]
pub fn poll_and_dispatch(
    messages: &[InboundMessage],
    messenger: &mut dyn MessageSink,
    sender: &mut SmsSender,
    router: &ReplyRouter,
    registry: &CommandRegistry,
    store: &mut dyn Store,
    log: &LogRing,
    modem_status: &ModemStatus,
    uptime_ms: u64,
    free_heap_bytes: u32,
    wifi_info: &str,
) -> Result<(bool, Option<u32>, OtaAction), MessengerError> {
    let mut restart_requested = false;
    let mut pause_mins: Option<u32> = None;
    let mut ota_action = OtaAction::None;

    for msg in messages {
        let text = msg.text.trim();

        if let Some(callback) = msg.callback.as_ref() {
            if let Some(offset) = log_cmd::parse_log_callback(&callback.data) {
                let ctx = CommandContext {
                    store: store as &dyn Store,
                    modem_status,
                    log_ring: log,
                    send_queue: sender,
                    uptime_ms,
                    free_heap_bytes,
                    wifi_info,
                };
                let page = log_cmd::render_log_page(&ctx, offset);
                messenger.edit_message_in_with_keyboard_and_format(
                    callback.conversation_id,
                    callback.message_id,
                    &page.text,
                    page.keyboard.as_ref(),
                    page.format,
                )?;
                messenger.answer_callback_query(&callback.id, None)?;
                continue;
            }
        }

        if text.starts_with('/') {
            if command_name(text) == Some("log") {
                let ctx = CommandContext {
                    store: store as &dyn Store,
                    modem_status,
                    log_ring: log,
                    send_queue: sender,
                    uptime_ms,
                    free_heap_bytes,
                    wifi_info,
                };
                let page = log_cmd::render_log_page(&ctx, 0);
                match msg.conversation_id {
                    Some(id) => messenger.send_message_to_with_keyboard_and_format(
                        id,
                        &page.text,
                        page.keyboard.as_ref(),
                        page.format,
                    )?,
                    None => messenger.send_message_to_with_keyboard_and_format(
                        0,
                        &page.text,
                        page.keyboard.as_ref(),
                        page.format,
                    )?,
                };
                continue;
            }
            // Bot command
            let ctx = CommandContext {
                store: store as &dyn Store,
                modem_status,
                log_ring: log,
                send_queue: sender,
                uptime_ms,
                free_heap_bytes,
                wifi_info,
            };
            if let Some(reply) = registry.dispatch(text, &ctx) {
                let (clean, should_restart, maybe_pause, ota) =
                    apply_sentinels(&reply, sender, store);
                if should_restart {
                    restart_requested = true;
                }
                if maybe_pause.is_some() {
                    pause_mins = maybe_pause;
                }
                if ota != OtaAction::None {
                    ota_action = ota;
                }
                let display = clean.trim();
                if !display.is_empty() {
                    let result = match msg.conversation_id {
                        Some(id) => messenger.send_message_to(id, display),
                        None => messenger.send_message(display),
                    };
                    if let Err(e) = result {
                        log::error!("[poller] command reply failed: {}", e);
                    }
                }
            }
        } else if let Some(reply_to_id) = msg.reply_to {
            // Reply to a forwarded SMS
            if let Some(phone) = router.lookup(reply_to_id) {
                let phone = phone.to_string();
                log::info!("[poller] reply to {} via SMS", phone);
                if sender.enqueue(phone, text.to_string()).is_none() {
                    log::warn!("[poller] queue full — reply dropped");
                }
            } else {
                log::warn!("[poller] reply_to={} not found in router", reply_to_id);
            }
        } else {
            log::debug!("[poller] non-command non-reply message ignored: {}", text);
        }
    }

    Ok((restart_requested, pause_mins, ota_action))
}

fn command_name(text: &str) -> Option<&str> {
    let command = text.strip_prefix('/')?.split_whitespace().next()?;
    Some(command.split('@').next().unwrap_or(command))
}

/// Parse sentinel lines from a command reply, apply their side effects, and return
/// `(display_text, restart_requested, pause_mins)`.
fn apply_sentinels(
    reply: &str,
    sender: &mut SmsSender,
    store: &mut dyn Store,
) -> (String, bool, Option<u32>, OtaAction) {
    let mut display = String::new();
    let mut restart = false;
    let mut pause_mins: Option<u32> = None;
    let mut ota = OtaAction::None;

    for line in reply.lines() {
        if let Some(rest) = line.strip_prefix(SEND_SENTINEL) {
            // Format: "+phone|body" — body may have \n/\r encoded as escape sequences.
            if let Some((phone, body_encoded)) = rest.split_once('|') {
                let body = crate::commands::decode_sentinel_body(body_encoded);
                log::info!("[poller] sentinel: enqueue SMS to {}", phone);
                match sender.enqueue_command_send(phone.to_string(), body) {
                    CmdSendResult::Enqueued(_) => {}
                    CmdSendResult::QueueFull => {
                        log::warn!("[poller] queue full — /send dropped");
                    }
                    CmdSendResult::RateLimited => {
                        push_display_line(&mut display, crate::i18n::send_rate_limited());
                    }
                }
            }
        } else if let Some(phone) = line.strip_prefix(BLOCK_SENTINEL) {
            log::info!("[poller] sentinel: block {}", phone);
            let _ = crate::bridge::forwarder::add_to_blocklist(phone, store);
        } else if let Some(phone) = line.strip_prefix(UNBLOCK_SENTINEL) {
            log::info!("[poller] sentinel: unblock {}", phone);
            let _ = crate::bridge::forwarder::remove_from_blocklist(phone, store);
        } else if let Some(rest) = line.strip_prefix(PAUSE_SENTINEL) {
            let mins: u32 = rest.trim().parse().unwrap_or(60);
            log::info!("[poller] sentinel: pause forwarding for {} min", mins);
            let _ = save_bool(store, keys::FWD_ENABLED, false);
            pause_mins = Some(mins);
        } else if line.starts_with(RESUME_SENTINEL) {
            log::info!("[poller] sentinel: resume forwarding");
            let _ = save_bool(store, keys::FWD_ENABLED, true);
        } else if line.starts_with(RESTART_SENTINEL) {
            log::info!("[poller] sentinel: restart requested");
            restart = true;
        } else if line.starts_with(UPDATE_SENTINEL) {
            log::info!("[poller] sentinel: OTA update requested");
            ota = OtaAction::Update;
        } else if line.starts_with(UPDATE_CONFIRM_SENTINEL) {
            log::info!("[poller] sentinel: OTA confirm requested");
            ota = OtaAction::Confirm;
        } else {
            push_display_line(&mut display, line);
        }
    }

    (display, restart, pause_mins, ota)
}

fn push_display_line(display: &mut String, line: &str) {
    if !display.is_empty() {
        display.push('\n');
    }
    display.push_str(line);
}

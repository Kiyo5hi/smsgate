//! SMS → IM forwarding core logic.

use crate::bridge::reply_router::ReplyRouter;
use crate::im::{MessageId, MessageSink};
use crate::log_ring::{LogEntry, LogRing};
use crate::persist::{keys, load_bool, Store};
use crate::sms::{codec::human_readable_phone, SmsMessage};

/// Process and forward one SMS. Returns the IM MessageId on success.
pub fn forward_sms(
    sms: &SmsMessage,
    messenger: &mut dyn MessageSink,
    router: &mut ReplyRouter,
    log: &mut LogRing,
    store: &mut dyn Store,
) -> Option<MessageId> {
    let log_entry = |forwarded| {
        LogEntry::sms(
            sms.sender.clone(),
            sms.body.chars().take(80).collect(),
            sms.timestamp.clone(),
            forwarded,
        )
    };

    if load_bool(store, keys::FWD_ENABLED) == Some(false) {
        log::info!(
            "[forwarder] forwarding paused — dropping SMS from {}",
            sms.sender
        );
        log.push(log_entry(false));
        return None;
    }

    if is_blocked(&sms.sender, store) {
        log::info!("[forwarder] {} is blocked — dropped", sms.sender);
        log.push(log_entry(false));
        return None;
    }

    let display = human_readable_phone(&sms.sender);
    let text = crate::i18n::sms_received_html(
        &html_escape(&display),
        &html_escape(&crate::sms::codec::timestamp_to_rfc3339(
            &sms.timestamp,
            480,
        )),
        &html_escape(&sms.body),
    );

    match messenger.send_message_with_format(&text, crate::im::MessageFormat::Html) {
        Ok(msg_id) => {
            router.put(msg_id, &sms.sender, store);
            log.push(log_entry(true));
            log::info!(
                "[forwarder] forwarded SMS from {} → msg_id={}",
                sms.sender,
                msg_id
            );
            Some(msg_id)
        }
        Err(e) => {
            log::error!("[forwarder] send failed: {}", e);
            None
        }
    }
}

fn html_escape(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    for ch in input.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            _ => out.push(ch),
        }
    }
    out
}

/// Check if `phone` is in the block list.
pub fn is_blocked(phone: &str, store: &dyn Store) -> bool {
    let Some(bytes) = store.load(keys::BLOCK_LIST) else {
        return false;
    };
    let Ok(list) = serde_json::from_slice::<Vec<String>>(&bytes) else {
        return false;
    };
    let normalized = crate::sms::codec::normalize_phone(phone);
    list.iter().any(|b| {
        let nb = crate::sms::codec::normalize_phone(b);
        nb == normalized || normalized.ends_with(&nb) || nb.ends_with(&normalized)
    })
}

/// Add a phone number to the block list.
pub fn add_to_blocklist(
    phone: &str,
    store: &mut dyn Store,
) -> Result<(), crate::persist::StoreError> {
    let mut list = load_blocklist(store);
    let n = crate::sms::codec::normalize_phone(phone);
    if !list.contains(&n) {
        list.push(n);
        save_blocklist(&list, store)?;
    }
    Ok(())
}

/// Remove a phone number from the block list.
pub fn remove_from_blocklist(phone: &str, store: &mut dyn Store) -> bool {
    let mut list = load_blocklist(store);
    let n = crate::sms::codec::normalize_phone(phone);
    let before = list.len();
    list.retain(|b| *b != n);
    if list.len() < before {
        let _ = save_blocklist(&list, store);
        true
    } else {
        false
    }
}

pub fn load_blocklist(store: &dyn Store) -> Vec<String> {
    store
        .load(keys::BLOCK_LIST)
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_default()
}

fn save_blocklist(
    list: &[String],
    store: &mut dyn Store,
) -> Result<(), crate::persist::StoreError> {
    let bytes =
        serde_json::to_vec(list).map_err(|e| crate::persist::StoreError::Serde(e.to_string()))?;
    store.save(keys::BLOCK_LIST, &bytes)
}

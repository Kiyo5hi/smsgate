//! SMS inbound processing: CMTI handling and boot-time sweep.
//!
//! Extracted from main.rs so it can be unit-tested against ScriptedModem.

use crate::bridge::{forwarder::forward_sms, reply_router::ReplyRouter};
use crate::im::MessageSink;
use crate::log_ring::LogRing;
use crate::modem::ModemPort;
use crate::persist::Store;
use crate::sms::{codec::parse_sms_pdu, concat::ConcatReassembler, SmsMessage};

/// Process a +CMTI notification: read the PDU, forward it, delete on success.
///
/// Returns `true` if the modem slot was deleted (forwarded OK or unparseable),
/// `false` if forwarding failed and the slot should be retried on next boot.
#[allow(clippy::too_many_arguments)]
pub fn handle_new_sms(
    mem: &str,
    index: u16,
    modem: &mut dyn ModemPort,
    router: &mut ReplyRouter,
    log: &mut LogRing,
    concat: &mut ConcatReassembler,
    messenger: &mut dyn MessageSink,
    store: &mut dyn Store,
) -> bool {
    log::info!("[sms_handler] +CMTI: mem={} index={}", mem, index);

    ensure_pdu_mode(modem);
    let _ = modem.send_at(&format!("+CPMS=\"{}\"", mem));

    let r = modem.send_at(&format!("+CMGR={}", index));
    let stored = match &r {
        Ok(resp) if resp.ok => {
            log::info!("[sms_handler] AT+CMGR={} body: {:?}", index, resp.body);
            parse_cmgr_response(index, &resp.body)
        }
        Ok(resp) => {
            log::warn!(
                "[sms_handler] AT+CMGR={} error: {}",
                index,
                resp.body.trim()
            );
            None
        }
        Err(e) => {
            log::warn!("[sms_handler] AT+CMGR={} failed: {:?}", index, e);
            None
        }
    };

    let Some(stored) = stored else {
        log::warn!(
            "[sms_handler] could not read SMS at mem={} slot={}",
            mem,
            index
        );
        return false;
    };

    let delete = process_stored_sms(stored, index, router, log, concat, messenger, store);
    if delete {
        let _ = modem.send_at(&format!("+CMGD={}", index));
    } else {
        log::warn!(
            "[sms_handler] forward failed — SMS stays at mem={} slot={}",
            mem,
            index
        );
    }
    delete
}

/// Sweep all stored SMS in one memory bank (AT+CMGL=4) at boot time.
pub fn sweep_one_storage(
    mem: &str,
    modem: &mut dyn ModemPort,
    router: &mut ReplyRouter,
    log: &mut LogRing,
    concat: &mut ConcatReassembler,
    messenger: &mut dyn MessageSink,
    store: &mut dyn Store,
) {
    ensure_pdu_mode(modem);
    let Some((cmd, resp)) = list_stored_sms(mem, modem) else {
        return;
    };
    log::info!(
        "[sms_handler] sweep {} AT{} body: {:?}",
        mem,
        cmd,
        resp.body
    );

    for (slot, stored) in parse_cmgl_response(&resp.body) {
        log::info!("[sms_handler] sweep found SMS in {} slot {}", mem, slot);
        let delete = process_stored_sms(stored, slot, router, log, concat, messenger, store);
        if delete {
            let _ = modem.send_at(&format!("+CMGD={}", slot));
        } else {
            log::warn!(
                "[sms_handler] sweep forward failed — SMS stays at {} slot {}",
                mem,
                slot
            );
        }
    }
}

enum StoredSms {
    Pdu(String),
    Decoded(SmsMessage),
}

enum ListHeader {
    Pdu {
        index: u16,
    },
    Text {
        index: u16,
        sender: String,
        timestamp: String,
    },
}

fn ensure_pdu_mode(modem: &mut dyn ModemPort) {
    match modem.send_at("+CMGF=0") {
        Ok(resp) if resp.ok => {}
        Ok(resp) => log::warn!("[sms_handler] AT+CMGF=0 error: {}", resp.body.trim()),
        Err(e) => log::warn!("[sms_handler] AT+CMGF=0 failed: {:?}", e),
    }
}

fn list_stored_sms(
    mem: &str,
    modem: &mut dyn ModemPort,
) -> Option<(&'static str, crate::modem::AtResponse)> {
    for cmd in ["+CMGL=4", "+CMGL=\"ALL\""] {
        match modem.send_at(cmd) {
            Ok(resp) if resp.ok => return Some((cmd, resp)),
            Ok(resp) => log::warn!(
                "[sms_handler] sweep {} AT{} error: {}",
                mem,
                cmd,
                resp.body.trim()
            ),
            Err(e) => log::warn!("[sms_handler] sweep {} AT{} failed: {:?}", mem, cmd, e),
        }
    }
    None
}

fn process_stored_sms(
    stored: StoredSms,
    slot: u16,
    router: &mut ReplyRouter,
    log: &mut LogRing,
    concat: &mut ConcatReassembler,
    messenger: &mut dyn MessageSink,
    store: &mut dyn Store,
) -> bool {
    match stored {
        StoredSms::Pdu(hex) => process_pdu_hex(&hex, slot, router, log, concat, messenger, store),
        StoredSms::Decoded(sms) => forward_sms(&sms, messenger, router, log, store).is_some(),
    }
}

fn parse_cmgr_response(index: u16, body: &str) -> Option<StoredSms> {
    let mut lines = body.lines().map(str::trim).filter(|line| !line.is_empty());
    let line = lines.next()?;
    let rest = line.strip_prefix("+CMGR:")?.trim();
    if rest.starts_with('"') {
        let fields = parse_at_csv(rest);
        if fields.len() < 4 {
            return None;
        }
        let sender = decode_modem_text(&fields[1]);
        let timestamp = fields[3].trim().to_string();
        let body = lines.collect::<Vec<_>>().join("\n");
        return Some(StoredSms::Decoded(SmsMessage {
            sender,
            body: decode_modem_text(&body),
            timestamp,
            slot: index,
        }));
    }
    lines.next().map(|hex| StoredSms::Pdu(hex.to_string()))
}

fn parse_cmgl_response(body: &str) -> Vec<(u16, StoredSms)> {
    let mut stored = Vec::new();
    let mut current: Option<(ListHeader, String)> = None;
    for line in body.lines().map(str::trim).filter(|line| !line.is_empty()) {
        if let Some(header) = parse_cmgl_header(line) {
            flush_list_entry(&mut stored, current.take());
            current = Some((header, String::new()));
        } else if let Some((_, text)) = current.as_mut() {
            if !text.is_empty() {
                text.push('\n');
            }
            text.push_str(line);
        }
    }
    flush_list_entry(&mut stored, current);
    stored
}

fn parse_cmgl_header(line: &str) -> Option<ListHeader> {
    let fields = parse_at_csv(line.strip_prefix("+CMGL:")?.trim());
    let index = fields.first()?.trim().parse().ok()?;
    if fields.len() >= 5 && !fields[1].chars().all(|ch| ch.is_ascii_digit()) {
        return Some(ListHeader::Text {
            index,
            sender: decode_modem_text(&fields[2]),
            timestamp: fields[4].trim().to_string(),
        });
    }
    Some(ListHeader::Pdu { index })
}

fn flush_list_entry(stored: &mut Vec<(u16, StoredSms)>, entry: Option<(ListHeader, String)>) {
    let Some((header, body)) = entry else { return };
    match header {
        ListHeader::Pdu { index } if !body.is_empty() => {
            let pdu = body.lines().next().unwrap_or_default().trim().to_string();
            stored.push((index, StoredSms::Pdu(pdu)));
        }
        ListHeader::Text {
            index,
            sender,
            timestamp,
        } => {
            stored.push((
                index,
                StoredSms::Decoded(SmsMessage {
                    sender,
                    body: decode_modem_text(&body),
                    timestamp,
                    slot: index,
                }),
            ));
        }
        ListHeader::Pdu { .. } => {}
    }
}

fn parse_at_csv(input: &str) -> Vec<String> {
    let mut fields = Vec::new();
    let mut current = String::new();
    let mut in_quotes = false;
    for ch in input.chars() {
        match ch {
            '"' => in_quotes = !in_quotes,
            ',' if !in_quotes => {
                fields.push(current.trim().to_string());
                current.clear();
            }
            _ => current.push(ch),
        }
    }
    fields.push(current.trim().to_string());
    fields
}

fn decode_modem_text(text: &str) -> String {
    decode_ucs2_hex(text).unwrap_or_else(|| text.to_string())
}

fn decode_ucs2_hex(text: &str) -> Option<String> {
    let hex: String = text.chars().filter(|ch| !ch.is_whitespace()).collect();
    if hex.len() < 4
        || !hex.len().is_multiple_of(4)
        || !hex.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        return None;
    }
    let mut units = Vec::with_capacity(hex.len() / 4);
    for chunk in hex.as_bytes().as_chunks::<4>().0 {
        let raw = std::str::from_utf8(chunk).ok()?;
        units.push(u16::from_str_radix(raw, 16).ok()?);
    }
    let plausible = units
        .iter()
        .filter(|&&unit| is_text_code_unit(unit))
        .count();
    if plausible * 2 < units.len() {
        return None;
    }
    std::char::decode_utf16(units)
        .collect::<Result<String, _>>()
        .ok()
        .filter(|decoded| !decoded.is_empty())
}

fn is_text_code_unit(unit: u16) -> bool {
    matches!(
        unit,
        0x0009 | 0x000A | 0x000D
            | 0x0020..=0x007E
            | 0x00A0..=0x00FF
            | 0x2000..=0x206F
            | 0x3000..=0x303F
            | 0x3400..=0x9FFF
            | 0xFF00..=0xFFEF
    )
}

/// Parse a PDU hex string and forward the SMS.
///
/// Returns `true` if the modem slot should be deleted:
/// - single SMS forwarded successfully
/// - concat partial (slot consumed; delete to free storage)
/// - unparseable PDU (no point retaining)
///
/// Returns `false` if forwarding failed (keep for retry on next boot).
pub fn process_pdu_hex(
    hex: &str,
    slot: u16,
    router: &mut ReplyRouter,
    log: &mut LogRing,
    concat: &mut ConcatReassembler,
    messenger: &mut dyn MessageSink,
    store: &mut dyn Store,
) -> bool {
    let pdu = match parse_sms_pdu(hex) {
        Ok(p) => p,
        Err(e) => {
            log::error!("[sms_handler] PDU parse error at slot {}: {}", slot, e);
            return true; // unparseable — no point keeping it
        }
    };

    let sms = if let Some(notification) = crate::mms::parse_mms_notification_from_sms(&pdu) {
        SmsMessage {
            sender: pdu.sender,
            body: crate::i18n::mms_notification(
                &notification.content_location,
                notification.message_size,
                notification.expiry,
            ),
            timestamp: pdu.timestamp,
            slot,
        }
    } else if pdu.is_concatenated {
        match concat.feed(&pdu) {
            Some(complete) => SmsMessage {
                sender: complete.sender,
                body: complete.content,
                timestamp: complete.timestamp,
                slot,
            },
            None => return true, // concat partial consumed; delete to free modem slot
        }
    } else {
        SmsMessage {
            sender: pdu.sender,
            body: pdu.content,
            timestamp: pdu.timestamp,
            slot,
        }
    };

    forward_sms(&sms, messenger, router, log, store).is_some()
}

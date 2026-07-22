//! MMS WAP Push notification parsing without downloading MMS content.

use crate::sms::codec::SmsPdu;

const WAP_PUSH_PORT: u16 = 2948;
const WSP_PUSH_PDU: u8 = 0x06;
const WSP_CONTENT_TYPE_MMS: u8 = 0xBE;
const MMS_MIME: &[u8] = b"application/vnd.wap.mms-message";
const MMS_HEADER_CONTENT_LOCATION: u8 = 0x83;
const MMS_HEADER_EXPIRY: u8 = 0x88;
const MMS_HEADER_MESSAGE_ID: u8 = 0x8B;
const MMS_HEADER_MESSAGE_TYPE: u8 = 0x8C;
const MMS_HEADER_MMS_VERSION: u8 = 0x8D;
const MMS_HEADER_MESSAGE_SIZE: u8 = 0x8E;
const MMS_HEADER_RESPONSE_TEXT: u8 = 0x93;
const MMS_HEADER_STATUS: u8 = 0x95;
const MMS_HEADER_SUBJECT: u8 = 0x96;
const MMS_HEADER_TO: u8 = 0x97;
const MMS_HEADER_TRANSACTION_ID: u8 = 0x98;
const MMS_MESSAGE_TYPE_NOTIFICATION_IND: u8 = 0x82;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MmsExpiry {
    AbsoluteUnixSeconds(u64),
    RelativeSeconds(u64),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MmsNotification {
    pub content_location: String,
    pub message_size: Option<u64>,
    pub expiry: Option<MmsExpiry>,
}

pub fn parse_mms_notification_from_sms(pdu: &SmsPdu) -> Option<MmsNotification> {
    if pdu.destination_port != Some(WAP_PUSH_PORT) || pdu.dcs != 0x04 {
        return None;
    }
    parse_wap_push_mms_notification(&pdu.user_data_payload)
}

fn parse_wap_push_mms_notification(payload: &[u8]) -> Option<MmsNotification> {
    let mut pos = 0;
    let _transaction_id = read_u8(payload, &mut pos)?;
    if read_u8(payload, &mut pos)? != WSP_PUSH_PDU {
        return None;
    }
    let header_len = read_uintvar(payload, &mut pos)? as usize;
    let headers = take(payload, &mut pos, header_len)?;
    if headers.first().copied() != Some(WSP_CONTENT_TYPE_MMS)
        && !headers
            .windows(MMS_MIME.len())
            .any(|window| window == MMS_MIME)
    {
        return None;
    }
    parse_mms_headers(&payload[pos..])
}

fn parse_mms_headers(bytes: &[u8]) -> Option<MmsNotification> {
    let mut pos = 0;
    let mut is_notification = false;
    let mut content_location = None;
    let mut message_size = None;
    let mut expiry = None;
    while pos < bytes.len() {
        match read_u8(bytes, &mut pos)? {
            MMS_HEADER_MESSAGE_TYPE => {
                is_notification = read_u8(bytes, &mut pos)? == MMS_MESSAGE_TYPE_NOTIFICATION_IND;
            }
            MMS_HEADER_CONTENT_LOCATION => content_location = read_text_string(bytes, &mut pos),
            MMS_HEADER_MESSAGE_SIZE => message_size = read_integer_value(bytes, &mut pos),
            MMS_HEADER_EXPIRY => expiry = read_expiry_value(bytes, &mut pos),
            MMS_HEADER_TRANSACTION_ID
            | MMS_HEADER_MESSAGE_ID
            | MMS_HEADER_RESPONSE_TEXT
            | MMS_HEADER_SUBJECT
            | MMS_HEADER_TO => {
                skip_text_or_value(bytes, &mut pos);
            }
            MMS_HEADER_MMS_VERSION | MMS_HEADER_STATUS => {
                let _ = read_u8(bytes, &mut pos);
            }
            _ => {
                if !skip_text_or_value(bytes, &mut pos) {
                    break;
                }
            }
        }
    }
    let content_location = content_location?;
    let valid_url = (content_location.starts_with("http://")
        || content_location.starts_with("https://"))
        && content_location.contains(".mms");
    (is_notification && valid_url).then_some(MmsNotification {
        content_location,
        message_size,
        expiry,
    })
}

fn read_expiry_value(bytes: &[u8], pos: &mut usize) -> Option<MmsExpiry> {
    let len = read_value_length(bytes, pos)?;
    let end = pos.checked_add(len)?;
    if end > bytes.len() || *pos >= end {
        return None;
    }
    let token = read_u8(bytes, pos)?;
    let value = read_integer_value(&bytes[..end], pos)?;
    *pos = end;
    match token {
        0x80 => Some(MmsExpiry::AbsoluteUnixSeconds(value)),
        0x81 => Some(MmsExpiry::RelativeSeconds(value)),
        _ => None,
    }
}

fn read_value_length(bytes: &[u8], pos: &mut usize) -> Option<usize> {
    match read_u8(bytes, pos)? {
        value @ 0..=30 => Some(value as usize),
        31 => read_uintvar(bytes, pos).map(|value| value as usize),
        _ => None,
    }
}

fn read_integer_value(bytes: &[u8], pos: &mut usize) -> Option<u64> {
    let first = read_u8(bytes, pos)?;
    if first & 0x80 != 0 {
        return Some((first & 0x7F) as u64);
    }
    let len = first as usize;
    if len == 0 || len > 8 {
        return None;
    }
    Some(
        take(bytes, pos, len)?
            .iter()
            .fold(0, |acc, byte| (acc << 8) | u64::from(*byte)),
    )
}

fn read_uintvar(bytes: &[u8], pos: &mut usize) -> Option<u64> {
    let mut value = 0;
    for _ in 0..5 {
        let byte = read_u8(bytes, pos)?;
        value = (value << 7) | u64::from(byte & 0x7F);
        if byte & 0x80 == 0 {
            return Some(value);
        }
    }
    None
}

fn read_text_string(bytes: &[u8], pos: &mut usize) -> Option<String> {
    if bytes.get(*pos) == Some(&0x7F) {
        *pos += 1;
    }
    let start = *pos;
    while *pos < bytes.len() && bytes[*pos] != 0 {
        *pos += 1;
    }
    if *pos >= bytes.len() {
        return None;
    }
    let text = std::str::from_utf8(&bytes[start..*pos]).ok()?.to_string();
    *pos += 1;
    Some(text)
}

fn skip_text_or_value(bytes: &[u8], pos: &mut usize) -> bool {
    if *pos >= bytes.len() {
        return false;
    }
    let saved = *pos;
    if let Some(len) = read_value_length(bytes, pos) {
        if pos.saturating_add(len) <= bytes.len() {
            *pos += len;
            return true;
        }
    }
    *pos = saved;
    if read_text_string(bytes, pos).is_some() {
        return true;
    }
    *pos = saved;
    read_u8(bytes, pos).is_some()
}

fn read_u8(bytes: &[u8], pos: &mut usize) -> Option<u8> {
    let byte = *bytes.get(*pos)?;
    *pos += 1;
    Some(byte)
}

fn take<'a>(bytes: &'a [u8], pos: &mut usize, len: usize) -> Option<&'a [u8]> {
    let end = pos.checked_add(len)?;
    let slice = bytes.get(*pos..end)?;
    *pos = end;
    Some(slice)
}

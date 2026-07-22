//! Tests for Telegram Bot API JSON deserialization (types.rs).

use smsgate::im::telegram::types::{json_escape, ApiResult, SendMessageResult, Update};
use smsgate::im::telegram::{build_message_body, is_trusted_chat, update_to_inbound_message};
use smsgate::im::{
    telegram::{poll_retry_after, send_retry_delay, should_retry_send, SEND_RETRY_INTERVAL},
    InlineKeyboard, InlineKeyboardButton, MessageFormat, MessengerError,
};

#[test]
fn primary_chat_is_trusted() {
    assert!(is_trusted_chat(10, &[20], 10));
}

#[test]
fn additional_chat_is_trusted() {
    assert!(is_trusted_chat(10, &[20, 30], 20));
}

#[test]
fn unknown_chat_is_rejected() {
    assert!(!is_trusted_chat(10, &[20, 30], 40));
}

#[test]
fn formatted_keyboard_body_is_valid_json() {
    let keyboard = InlineKeyboard::single_row(vec![InlineKeyboardButton::new("Older", "log:16")]);
    let raw = build_message_body(
        10,
        Some(77),
        "<b>events</b>",
        Some(&keyboard),
        MessageFormat::Html,
    );
    let value: serde_json::Value = serde_json::from_str(&raw).unwrap();
    assert_eq!(value["chat_id"], 10);
    assert_eq!(value["message_id"], 77);
    assert_eq!(value["parse_mode"], "HTML");
    assert_eq!(
        value["reply_markup"]["inline_keyboard"][0][0]["callback_data"],
        "log:16"
    );
}

// ---------------------------------------------------------------------------
// json_escape — used by send_message to build valid JSON bodies
// ---------------------------------------------------------------------------

#[test]
fn json_escape_plain_text() {
    assert_eq!(json_escape("hello"), "hello");
}

#[test]
fn json_escape_newline() {
    // SMS forward messages contain literal \n — must become \\n in JSON
    assert_eq!(json_escape("line1\nline2"), "line1\\nline2");
}

#[test]
fn json_escape_carriage_return() {
    assert_eq!(json_escape("a\rb"), "a\\rb");
}

#[test]
fn json_escape_tab() {
    assert_eq!(json_escape("a\tb"), "a\\tb");
}

#[test]
fn json_escape_backslash() {
    assert_eq!(json_escape("a\\b"), "a\\\\b");
}

#[test]
fn json_escape_double_quote() {
    assert_eq!(json_escape("say \"hi\""), "say \\\"hi\\\"");
}

#[test]
fn json_escape_sms_forward_format() {
    // Matches the actual format used in forward_sms:
    // format!("📱 SMS from {}\n🕐 {}\n\n{}", sender, ts, body)
    let text = "📱 SMS from +8613812345678\n🕐 2024-01-01 12:00\n\nHello world";
    let escaped = json_escape(text);
    // Verify the result can be embedded in a JSON string without breaking parsing
    let json = format!(r#"{{"text":"{}"}}"#, escaped);
    let v: serde_json::Value = serde_json::from_str(&json).expect("must be valid JSON");
    assert_eq!(v["text"].as_str().unwrap(), text);
}

#[test]
fn json_escape_all_special_chars_combined() {
    let text = "a\nb\\c\"d\re\tf";
    let escaped = json_escape(text);
    let json = format!(r#"{{"t":"{}"}}"#, escaped);
    let v: serde_json::Value = serde_json::from_str(&json).expect("must be valid JSON");
    assert_eq!(v["t"].as_str().unwrap(), text);
}

// ---------------------------------------------------------------------------
// ApiResult<bool> — setMyCommands / deleteMyCommands responses
// ---------------------------------------------------------------------------

#[test]
fn api_result_ok_true() {
    let json = r#"{"ok":true,"result":true}"#;
    let r: ApiResult<bool> = serde_json::from_str(json).unwrap();
    assert!(r.ok);
    assert_eq!(r.result, Some(true));
    assert!(r.description.is_none());
}

#[test]
fn api_result_ok_false_with_description() {
    let json = r#"{"ok":false,"description":"Unauthorized"}"#;
    let r: ApiResult<bool> = serde_json::from_str(json).unwrap();
    assert!(!r.ok);
    assert!(r.result.is_none());
    assert_eq!(r.description.as_deref(), Some("Unauthorized"));
}

// ---------------------------------------------------------------------------
// ApiResult<SendMessageResult> — sendMessage response
// ---------------------------------------------------------------------------

#[test]
fn send_message_result_extracts_message_id() {
    let json = r#"{"ok":true,"result":{"message_id":42,"chat":{"id":123}}}"#;
    let r: ApiResult<SendMessageResult> = serde_json::from_str(json).unwrap();
    assert!(r.ok);
    assert_eq!(r.result.unwrap().message_id, 42);
}

#[test]
fn send_message_result_api_error() {
    let json = r#"{"ok":false,"description":"Bad Request: chat not found"}"#;
    let r: ApiResult<SendMessageResult> = serde_json::from_str(json).unwrap();
    assert!(!r.ok);
    assert!(r.result.is_none());
    assert!(r.description.unwrap().contains("chat not found"));
}

// ---------------------------------------------------------------------------
// ApiResult<Vec<Update>> — getUpdates response
// ---------------------------------------------------------------------------

#[test]
fn get_updates_empty_result() {
    let json = r#"{"ok":true,"result":[]}"#;
    let r: ApiResult<Vec<Update>> = serde_json::from_str(json).unwrap();
    assert!(r.ok);
    assert_eq!(r.result.unwrap().len(), 0);
}

#[test]
fn get_updates_single_message() {
    let json = r#"{
        "ok": true,
        "result": [{
            "update_id": 100,
            "message": {
                "message_id": 5,
                "text": "Hello",
                "chat": {"id": 987654321}
            }
        }]
    }"#;
    let r: ApiResult<Vec<Update>> = serde_json::from_str(json).unwrap();
    assert!(r.ok);
    let updates = r.result.unwrap();
    assert_eq!(updates.len(), 1);
    let u = &updates[0];
    assert_eq!(u.update_id, 100);
    let msg = u.message.as_ref().unwrap();
    assert_eq!(msg.message_id, 5);
    assert_eq!(msg.text.as_deref(), Some("Hello"));
    assert!(msg.reply_to_message.is_none());
}

#[test]
fn get_updates_with_reply_to() {
    let json = r#"{
        "ok": true,
        "result": [{
            "update_id": 200,
            "message": {
                "message_id": 10,
                "text": "/send +1 hi",
                "chat": {"id": 111},
                "reply_to_message": {"message_id": 9}
            }
        }]
    }"#;
    let r: ApiResult<Vec<Update>> = serde_json::from_str(json).unwrap();
    let updates = r.result.unwrap();
    let msg = updates[0].message.as_ref().unwrap();
    assert_eq!(msg.reply_to_message.as_ref().unwrap().message_id, 9);
}

#[test]
fn get_updates_message_without_text() {
    // Non-text messages (stickers, photos, etc.) arrive with no "text" field
    let json = r#"{
        "ok": true,
        "result": [{
            "update_id": 300,
            "message": {
                "message_id": 20,
                "chat": {"id": 111}
            }
        }]
    }"#;
    let r: ApiResult<Vec<Update>> = serde_json::from_str(json).unwrap();
    let updates = r.result.unwrap();
    let msg = updates[0].message.as_ref().unwrap();
    assert!(msg.text.is_none());
}

#[test]
fn get_updates_no_message_field() {
    // Non-message updates (channel_post, etc.) may omit the message field
    let json = r#"{
        "ok": true,
        "result": [{"update_id": 400}]
    }"#;
    let r: ApiResult<Vec<Update>> = serde_json::from_str(json).unwrap();
    let updates = r.result.unwrap();
    assert!(updates[0].message.is_none());
}

#[test]
fn get_updates_multiple_messages() {
    let json = r#"{
        "ok": true,
        "result": [
            {"update_id": 1, "message": {"message_id": 1, "text": "a", "chat": {"id": 1}}},
            {"update_id": 2, "message": {"message_id": 2, "text": "b", "chat": {"id": 1}}}
        ]
    }"#;
    let r: ApiResult<Vec<Update>> = serde_json::from_str(json).unwrap();
    let updates = r.result.unwrap();
    assert_eq!(updates.len(), 2);
    assert_eq!(updates[0].update_id, 1);
    assert_eq!(updates[1].update_id, 2);
}

#[test]
fn get_updates_api_error() {
    let json = r#"{"ok":false,"description":"Too Many Requests: retry after 30"}"#;
    let r: ApiResult<Vec<Update>> = serde_json::from_str(json).unwrap();
    assert!(!r.ok);
    assert!(r.result.is_none());
    assert!(r.description.unwrap().contains("Too Many Requests"));
}

#[test]
fn update_deserializes_document_and_callback() {
    let document_json = r#"{"update_id":5,"message":{"message_id":8,"caption":"/ota","document":{"file_id":"f","file_unique_id":"u","file_name":"fw.bin","file_size":123},"chat":{"id":10}}}"#;
    let update: Update = serde_json::from_str(document_json).unwrap();
    assert_eq!(
        update
            .message
            .unwrap()
            .document
            .unwrap()
            .file_name
            .as_deref(),
        Some("fw.bin")
    );

    let callback_json = r#"{"update_id":6,"callback_query":{"id":"cb","data":"log:16","message":{"message_id":9,"chat":{"id":10}}}}"#;
    let update: Update = serde_json::from_str(callback_json).unwrap();
    assert_eq!(
        update.callback_query.unwrap().data.as_deref(),
        Some("log:16")
    );
}

#[test]
fn converts_trusted_ota_document_and_callback() {
    let document: Update = serde_json::from_str(
        r#"{"update_id":5,"message":{"message_id":8,"chat":{"id":20},"caption":"/ota","document":{"file_id":"fw","file_unique_id":"fw-unique","file_name":"smsgate.bin","file_size":1234}}}"#,
    )
    .unwrap();
    let inbound = update_to_inbound_message(document, 10, &[20]).unwrap();
    assert_eq!(inbound.conversation_id, Some(20));
    assert_eq!(inbound.text, "/ota");
    assert_eq!(inbound.document.unwrap().file_id, "fw");

    let callback: Update = serde_json::from_str(
        r#"{"update_id":6,"callback_query":{"id":"cb","data":"log:16","message":{"message_id":9,"chat":{"id":20}}}}"#,
    )
    .unwrap();
    let inbound = update_to_inbound_message(callback, 10, &[20]).unwrap();
    let callback = inbound.callback.unwrap();
    assert_eq!(callback.conversation_id, 20);
    assert_eq!(callback.data, "log:16");
}

#[test]
fn rejects_untrusted_document_updates() {
    let update: Update = serde_json::from_str(
        r#"{"update_id":5,"message":{"message_id":8,"chat":{"id":30},"caption":"/ota","document":{"file_id":"fw","file_unique_id":"fw-unique"}}}"#,
    )
    .unwrap();
    assert!(update_to_inbound_message(update, 10, &[20]).is_none());
}

#[test]
fn api_result_extracts_retry_after() {
    let json = r#"{"ok":false,"description":"Too Many Requests","parameters":{"retry_after":37}}"#;
    let r: ApiResult<Vec<Update>> = serde_json::from_str(json).unwrap();
    assert_eq!(r.parameters.unwrap().retry_after, Some(37));
}

#[test]
fn poll_retry_after_uses_structured_rate_limit() {
    let error = MessengerError::RateLimited {
        retry_after_secs: 42,
        description: "Too Many Requests".into(),
    };
    assert_eq!(poll_retry_after(&error).unwrap().as_secs(), 42);
}

#[test]
fn poll_retry_after_accepts_legacy_description() {
    let error = MessengerError::Api("Too Many Requests: retry after 19".into());
    assert_eq!(poll_retry_after(&error).unwrap().as_secs(), 19);
}

#[test]
fn send_retry_policy_retries_transport_but_not_api_errors() {
    assert!(should_retry_send(&MessengerError::Http("reset".into())));
    assert!(should_retry_send(&MessengerError::Disconnected));
    assert!(!should_retry_send(&MessengerError::Api(
        "bad request".into()
    )));
}

#[test]
fn send_retry_policy_honors_rate_limit() {
    let error = MessengerError::RateLimited {
        retry_after_secs: 42,
        description: "slow down".into(),
    };
    assert_eq!(send_retry_delay(&error), std::time::Duration::from_secs(42));
    assert_eq!(
        send_retry_delay(&MessengerError::Http("reset".into())),
        SEND_RETRY_INTERVAL
    );
}

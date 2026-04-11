//! Tests for Telegram Bot API JSON deserialization (types.rs).

use smsgate::im::telegram::types::{ApiResult, SendMessageResult, Update};

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

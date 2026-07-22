use smsgate::im::{InboundDocument, InboundMessage};
use smsgate::ota::{is_ota_caption, latest_ota_document_cursor};

fn document(cursor: i64, caption: &str) -> InboundMessage {
    InboundMessage {
        cursor,
        text: caption.into(),
        reply_to: None,
        conversation_id: Some(1),
        callback: None,
        document: Some(InboundDocument {
            file_id: "id".into(),
            file_unique_id: "unique".into(),
            file_name: Some("smsgate.bin".into()),
            mime_type: Some("application/octet-stream".into()),
            file_size: Some(1024),
            caption: Some(caption.into()),
        }),
    }
}

#[test]
fn ota_caption_accepts_bot_suffix() {
    assert!(is_ota_caption("/ota@my_bot install"));
    assert!(!is_ota_caption("/update"));
}

#[test]
fn newest_ota_document_wins() {
    assert_eq!(
        latest_ota_document_cursor(&[document(4, "/ota"), document(9, "/ota")]),
        Some(9)
    );
}

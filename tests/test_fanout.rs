//! Tests for the FanoutSink dispatcher.

use smsgate::im::{MessageId, MessageSink, MessengerError};
use smsgate::im::fanout::FanoutSink;

struct CaptureSink {
    messages: Vec<String>,
    next_id: i64,
}

impl CaptureSink {
    fn new(start_id: i64) -> Self {
        CaptureSink { messages: Vec::new(), next_id: start_id }
    }
}

impl MessageSink for CaptureSink {
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError> {
        let id = self.next_id;
        self.next_id += 1;
        self.messages.push(text.to_string());
        Ok(id)
    }
}

struct FailSink;

impl MessageSink for FailSink {
    fn send_message(&mut self, _text: &str) -> Result<MessageId, MessengerError> {
        Err(MessengerError::Http("simulated".into()))
    }
}

#[test]
fn single_sink_returns_primary_id() {
    let mut fanout = FanoutSink::new(vec![Box::new(CaptureSink::new(100))]);
    let id = fanout.send_message("hello").unwrap();
    assert_eq!(id, 100);
}

#[test]
fn fanout_delivers_to_all_sinks() {
    let s1 = CaptureSink::new(1);
    let s2 = CaptureSink::new(200);
    let mut fanout = FanoutSink::new(vec![Box::new(s1), Box::new(s2)]);
    let id = fanout.send_message("test").unwrap();
    assert_eq!(id, 1, "primary sink ID should be returned");
    assert_eq!(fanout.sink_count(), 2);
}

#[test]
fn secondary_failure_does_not_fail_primary() {
    let s1 = CaptureSink::new(10);
    let mut fanout = FanoutSink::new(vec![Box::new(s1), Box::new(FailSink)]);
    let result = fanout.send_message("msg");
    assert!(result.is_ok());
    assert_eq!(result.unwrap(), 10);
}

#[test]
fn primary_failure_propagates() {
    let s2 = CaptureSink::new(50);
    let mut fanout = FanoutSink::new(vec![Box::new(FailSink), Box::new(s2)]);
    let result = fanout.send_message("msg");
    assert!(result.is_err());
}

#[test]
#[should_panic(expected = "at least one sink")]
fn empty_sinks_panics() {
    FanoutSink::new(vec![]);
}

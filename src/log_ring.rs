//! Persistent fixed-record event history with a volatile test fallback.

use std::cell::RefCell;
use thiserror::Error;

pub const FLASH_LOG_RECORD_SIZE: usize = 256;
pub const LOG_PAGE_SIZE: usize = 5;
const HEADER_SIZE: usize = 16;
const PAYLOAD_SIZE: usize = FLASH_LOG_RECORD_SIZE - HEADER_SIZE;
const MAGIC: u32 = 0x534D_4C47; // SMLG
const RAM_LOG_BYTES: usize = FLASH_LOG_RECORD_SIZE * 64;
const RAM_ERASE_BYTES: usize = 4096;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LogKind {
    Sms,
    Call,
    System,
    Network,
    User,
    Ota,
}

impl LogKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Sms => "sms",
            Self::Call => "call",
            Self::System => "system",
            Self::Network => "network",
            Self::User => "user",
            Self::Ota => "ota",
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Self::Sms => "SMS",
            Self::Call => "CALL",
            Self::System => "SYS",
            Self::Network => "NET",
            Self::User => "USER",
            Self::Ota => "OTA",
        }
    }
    fn parse(value: &str) -> Option<Self> {
        match value {
            "sms" => Some(Self::Sms),
            "call" => Some(Self::Call),
            "system" => Some(Self::System),
            "network" => Some(Self::Network),
            "user" => Some(Self::User),
            "ota" => Some(Self::Ota),
            _ => None,
        }
    }
}

/// A single entry in the SMS and runtime history log.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogEntry {
    pub kind: LogKind,
    pub sender: String,
    pub body_preview: String,
    pub timestamp: String,
    pub forwarded: bool,
}

impl LogEntry {
    pub fn event(subject: &str, detail: &str, ok: bool) -> Self {
        Self {
            kind: LogKind::System,
            sender: subject.to_string(),
            body_preview: detail.to_string(),
            timestamp: "-".to_string(),
            forwarded: ok,
        }
    }

    pub fn sms(sender: String, body_preview: String, timestamp: String, forwarded: bool) -> Self {
        Self {
            kind: LogKind::Sms,
            sender,
            body_preview,
            timestamp,
            forwarded,
        }
    }

    pub fn runtime(
        kind: LogKind,
        subject: &str,
        detail: &str,
        timestamp: String,
        ok: bool,
    ) -> Self {
        Self {
            kind,
            sender: subject.to_string(),
            body_preview: detail.to_string(),
            timestamp,
            forwarded: ok,
        }
    }

    fn encode(&self) -> Vec<u8> {
        let status = if self.forwarded { "1" } else { "0" };
        let timestamp_len = escaped_len(&self.timestamp);
        let kind = self.kind.as_str();
        let fixed = 2 + kind.len() + status.len() + 4 + timestamp_len;
        let available = PAYLOAD_SIZE.saturating_sub(fixed);
        let sender_len = escaped_len(&self.sender).min(available);
        let body_len = available.saturating_sub(sender_len);
        let mut out = String::with_capacity(PAYLOAD_SIZE);
        out.push_str("2\t");
        out.push_str(kind);
        out.push('\t');
        out.push_str(status);
        out.push('\t');
        push_escaped(&mut out, &self.timestamp, timestamp_len);
        out.push('\t');
        push_escaped(&mut out, &self.sender, sender_len);
        out.push('\t');
        push_escaped(&mut out, &self.body_preview, body_len);
        out.into_bytes()
    }

    fn decode(bytes: &[u8]) -> Option<Self> {
        let text = core::str::from_utf8(bytes).ok()?;
        let mut fields = text.split('\t');
        let first = fields.next()?;
        let (kind, forwarded) = if first == "2" {
            (LogKind::parse(fields.next()?)?, fields.next()? == "1")
        } else {
            (LogKind::Sms, first == "1")
        };
        let timestamp = unescape(fields.next()?);
        let sender = unescape(fields.next()?);
        let body_preview = unescape(fields.next()?);
        if fields.next().is_some() {
            return None;
        }
        Some(Self {
            kind,
            sender,
            body_preview,
            timestamp,
            forwarded,
        })
    }
}

pub struct LogRing {
    inner: RefCell<FlashLogRing<Box<dyn LogFlashStorage>>>,
}

impl LogRing {
    pub fn new() -> Self {
        Self::with_flash(Box::new(MemFlashLogStorage::new(
            RAM_LOG_BYTES,
            RAM_ERASE_BYTES,
        )))
        .expect("volatile log geometry is valid")
    }

    pub fn with_flash(storage: Box<dyn LogFlashStorage>) -> Result<Self, FlashLogError> {
        Ok(Self {
            inner: RefCell::new(FlashLogRing::mount(storage)?),
        })
    }

    pub fn push(&mut self, entry: LogEntry) {
        if let Err(e) = self.inner.borrow_mut().append(&entry) {
            log::warn!("[log] append failed: {}", e);
        }
    }

    pub fn last_n(&self, n: usize) -> Vec<LogEntry> {
        self.inner.borrow_mut().last_n(n).unwrap_or_else(|e| {
            log::warn!("[log] read failed: {}", e);
            Vec::new()
        })
    }

    /// Return newest-to-oldest entries after skipping `offset` newest records.
    pub fn page(&self, offset: usize, limit: usize) -> Result<Vec<LogEntry>, FlashLogError> {
        self.inner.borrow_mut().page(offset, limit)
    }

    pub fn len(&self) -> usize {
        self.inner.borrow_mut().count().unwrap_or_else(|e| {
            log::warn!("[log] count failed: {}", e);
            0
        })
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl Default for LogRing {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug, Error)]
pub enum FlashLogError {
    #[error("flash log storage out of bounds")]
    OutOfBounds,
    #[error("invalid flash log geometry: size={size}, erase_size={erase_size}")]
    InvalidGeometry { size: usize, erase_size: usize },
    #[error("flash log partition not found: {0}")]
    PartitionNotFound(String),
    #[error("flash log storage error: {0}")]
    Storage(String),
}

pub trait LogFlashStorage {
    fn len(&self) -> usize;
    fn is_empty(&self) -> bool {
        self.len() == 0
    }
    fn erase_size(&self) -> usize;
    fn read(&mut self, offset: usize, data: &mut [u8]) -> Result<(), FlashLogError>;
    fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashLogError>;
    fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashLogError>;
}

impl<T: LogFlashStorage + ?Sized> LogFlashStorage for Box<T> {
    fn len(&self) -> usize {
        (**self).len()
    }
    fn erase_size(&self) -> usize {
        (**self).erase_size()
    }
    fn read(&mut self, offset: usize, data: &mut [u8]) -> Result<(), FlashLogError> {
        (**self).read(offset, data)
    }
    fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashLogError> {
        (**self).write(offset, data)
    }
    fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashLogError> {
        (**self).erase(offset, len)
    }
}

#[derive(Clone, Copy)]
struct Header {
    seq: u32,
    slot: usize,
}

pub struct FlashLogRing<S: LogFlashStorage> {
    storage: S,
    slots: usize,
    slots_per_erase: usize,
    next_slot: usize,
    next_seq: u32,
}

impl<S: LogFlashStorage> FlashLogRing<S> {
    pub fn mount(mut storage: S) -> Result<Self, FlashLogError> {
        let size = storage.len();
        let erase_size = storage.erase_size();
        if size < erase_size
            || !size.is_multiple_of(erase_size)
            || !erase_size.is_multiple_of(FLASH_LOG_RECORD_SIZE)
        {
            return Err(FlashLogError::InvalidGeometry { size, erase_size });
        }
        let slots = size / FLASH_LOG_RECORD_SIZE;
        let slots_per_erase = erase_size / FLASH_LOG_RECORD_SIZE;
        let newest = scan_headers(&mut storage, slots)?
            .into_iter()
            .max_by_key(|h| h.seq);
        let (next_seq, next_slot) = newest
            .map(|h| (h.seq.wrapping_add(1).max(1), (h.slot + 1) % slots))
            .unwrap_or((1, 0));
        Ok(Self {
            storage,
            slots,
            slots_per_erase,
            next_slot,
            next_seq,
        })
    }

    pub fn append(&mut self, entry: &LogEntry) -> Result<(), FlashLogError> {
        if self.next_slot.is_multiple_of(self.slots_per_erase) {
            self.storage.erase(
                self.next_slot * FLASH_LOG_RECORD_SIZE,
                self.storage.erase_size(),
            )?;
        }
        let payload = entry.encode();
        let mut record = [0xFF; FLASH_LOG_RECORD_SIZE];
        record[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        record[4..8].copy_from_slice(&self.next_seq.to_le_bytes());
        record[8..10].copy_from_slice(&(payload.len() as u16).to_le_bytes());
        record[10..14].copy_from_slice(&checksum(&payload).to_le_bytes());
        record[HEADER_SIZE..HEADER_SIZE + payload.len()].copy_from_slice(&payload);
        self.storage
            .write(self.next_slot * FLASH_LOG_RECORD_SIZE, &record)?;
        self.next_slot = (self.next_slot + 1) % self.slots;
        self.next_seq = self.next_seq.wrapping_add(1).max(1);
        Ok(())
    }

    pub fn last_n(&mut self, n: usize) -> Result<Vec<LogEntry>, FlashLogError> {
        let mut headers = scan_headers(&mut self.storage, self.slots)?;
        headers.sort_unstable_by_key(|h| h.seq);
        let skip = headers.len().saturating_sub(n);
        headers
            .into_iter()
            .skip(skip)
            .filter_map(|h| {
                match read_entry(&mut self.storage, h.slot) {
                    Ok(entry) => entry,
                    Err(e) => return Some(Err(e)),
                }
                .map(Ok)
            })
            .collect()
    }

    pub fn count(&mut self) -> Result<usize, FlashLogError> {
        Ok(scan_headers(&mut self.storage, self.slots)?.len())
    }

    pub fn page(&mut self, offset: usize, limit: usize) -> Result<Vec<LogEntry>, FlashLogError> {
        let mut headers = scan_headers(&mut self.storage, self.slots)?;
        headers.sort_unstable_by_key(|h| h.seq);
        let mut entries = Vec::with_capacity(limit.min(headers.len().saturating_sub(offset)));
        for header in headers.iter().rev().skip(offset).take(limit) {
            if let Some(entry) = read_entry(&mut self.storage, header.slot)? {
                entries.push(entry);
            }
        }
        Ok(entries)
    }

    pub fn into_storage(self) -> S {
        self.storage
    }
}

fn scan_headers<S: LogFlashStorage>(
    storage: &mut S,
    slots: usize,
) -> Result<Vec<Header>, FlashLogError> {
    let mut out = Vec::new();
    for slot in 0..slots {
        if let Some((seq, _)) = read_meta(storage, slot)? {
            out.push(Header { seq, slot });
        }
    }
    Ok(out)
}

fn read_entry<S: LogFlashStorage>(
    storage: &mut S,
    slot: usize,
) -> Result<Option<LogEntry>, FlashLogError> {
    let mut record = [0; FLASH_LOG_RECORD_SIZE];
    storage.read(slot * FLASH_LOG_RECORD_SIZE, &mut record)?;
    let Some((_, len)) = validate_record(&record) else {
        return Ok(None);
    };
    Ok(LogEntry::decode(&record[HEADER_SIZE..HEADER_SIZE + len]))
}

fn read_meta<S: LogFlashStorage>(
    storage: &mut S,
    slot: usize,
) -> Result<Option<(u32, usize)>, FlashLogError> {
    let mut record = [0; FLASH_LOG_RECORD_SIZE];
    storage.read(slot * FLASH_LOG_RECORD_SIZE, &mut record)?;
    Ok(validate_record(&record))
}

fn validate_record(record: &[u8; FLASH_LOG_RECORD_SIZE]) -> Option<(u32, usize)> {
    if u32::from_le_bytes(record[0..4].try_into().ok()?) != MAGIC {
        return None;
    }
    let seq = u32::from_le_bytes(record[4..8].try_into().ok()?);
    let len = u16::from_le_bytes(record[8..10].try_into().ok()?) as usize;
    let expected = u32::from_le_bytes(record[10..14].try_into().ok()?);
    if seq == 0 || len > PAYLOAD_SIZE {
        return None;
    }
    let payload = &record[HEADER_SIZE..HEADER_SIZE + len];
    (checksum(payload) == expected).then_some((seq, len))
}

fn checksum(bytes: &[u8]) -> u32 {
    bytes.iter().fold(0x811C_9DC5u32, |hash, byte| {
        (hash ^ u32::from(*byte)).wrapping_mul(0x0100_0193)
    })
}

fn escaped_len(input: &str) -> usize {
    input
        .chars()
        .map(|c| {
            if matches!(c, '\\' | '\t' | '\n' | '\r') {
                2
            } else {
                c.len_utf8()
            }
        })
        .sum()
}

fn push_escaped(out: &mut String, input: &str, budget: usize) {
    let mut used = 0;
    for c in input.chars() {
        let encoded = match c {
            '\\' => "\\\\",
            '\t' => "\\t",
            '\n' => "\\n",
            '\r' => "\\r",
            _ => "",
        };
        let size = if encoded.is_empty() { c.len_utf8() } else { 2 };
        if used + size > budget {
            break;
        }
        if encoded.is_empty() {
            out.push(c);
        } else {
            out.push_str(encoded);
        }
        used += size;
    }
}

fn unescape(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let mut chars = input.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('t') => out.push('\t'),
            Some('n') => out.push('\n'),
            Some('r') => out.push('\r'),
            Some('\\') => out.push('\\'),
            Some(other) => {
                out.push('\\');
                out.push(other);
            }
            None => out.push('\\'),
        }
    }
    out
}

#[derive(Debug, Clone)]
pub struct MemFlashLogStorage {
    data: Vec<u8>,
    erase_size: usize,
}

impl MemFlashLogStorage {
    pub fn new(size: usize, erase_size: usize) -> Self {
        Self {
            data: vec![0xFF; size],
            erase_size,
        }
    }
    pub fn corrupt_byte(&mut self, offset: usize) {
        self.data[offset] ^= 0x55;
    }
}

impl LogFlashStorage for MemFlashLogStorage {
    fn len(&self) -> usize {
        self.data.len()
    }
    fn erase_size(&self) -> usize {
        self.erase_size
    }
    fn read(&mut self, offset: usize, data: &mut [u8]) -> Result<(), FlashLogError> {
        let src = self
            .data
            .get(
                offset
                    ..offset
                        .checked_add(data.len())
                        .ok_or(FlashLogError::OutOfBounds)?,
            )
            .ok_or(FlashLogError::OutOfBounds)?;
        data.copy_from_slice(src);
        Ok(())
    }
    fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashLogError> {
        let dst = self
            .data
            .get_mut(
                offset
                    ..offset
                        .checked_add(data.len())
                        .ok_or(FlashLogError::OutOfBounds)?,
            )
            .ok_or(FlashLogError::OutOfBounds)?;
        for (old, new) in dst.iter_mut().zip(data) {
            *old &= *new;
        }
        Ok(())
    }
    fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashLogError> {
        if !offset.is_multiple_of(self.erase_size) || !len.is_multiple_of(self.erase_size) {
            return Err(FlashLogError::InvalidGeometry {
                size: self.data.len(),
                erase_size: self.erase_size,
            });
        }
        self.data
            .get_mut(offset..offset.checked_add(len).ok_or(FlashLogError::OutOfBounds)?)
            .ok_or(FlashLogError::OutOfBounds)?
            .fill(0xFF);
        Ok(())
    }
}

#[cfg(feature = "esp32")]
pub struct EspFlashLogStorage {
    partition: esp_idf_svc::partition::EspPartition,
    size: usize,
    erase_size: usize,
}

#[cfg(feature = "esp32")]
impl EspFlashLogStorage {
    pub fn open(label: &str) -> Result<Self, FlashLogError> {
        let partition = unsafe {
            esp_idf_svc::partition::EspPartition::new(label)
                .map_err(|e| FlashLogError::Storage(e.to_string()))?
        }
        .ok_or_else(|| FlashLogError::PartitionNotFound(label.to_string()))?;
        Ok(Self {
            size: partition.size(),
            erase_size: partition.erase_size(),
            partition,
        })
    }
}

#[cfg(feature = "esp32")]
impl LogFlashStorage for EspFlashLogStorage {
    fn len(&self) -> usize {
        self.size
    }
    fn erase_size(&self) -> usize {
        self.erase_size
    }
    fn read(&mut self, offset: usize, data: &mut [u8]) -> Result<(), FlashLogError> {
        self.partition
            .read(offset, data)
            .map_err(|e| FlashLogError::Storage(e.to_string()))
    }
    fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashLogError> {
        self.partition
            .write(offset, data)
            .map_err(|e| FlashLogError::Storage(e.to_string()))
    }
    fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashLogError> {
        self.partition
            .erase(offset, len)
            .map_err(|e| FlashLogError::Storage(e.to_string()))
    }
}

#[cfg(feature = "esp32")]
pub fn open_flash_log_ring(label: &str) -> Result<LogRing, FlashLogError> {
    LogRing::with_flash(Box::new(EspFlashLogStorage::open(label)?))
}

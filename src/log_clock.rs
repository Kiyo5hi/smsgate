//! Timestamp source for firmware-generated persistent log entries.

use crate::timer::elapsed_since;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NetworkDateTime {
    pub year: u16,
    pub month: u8,
    pub day: u8,
    pub hour: u8,
    pub minute: u8,
    pub second: u8,
    pub offset_minutes: i32,
}

impl NetworkDateTime {
    pub fn format(&self) -> String {
        format_datetime(
            self.year as i32,
            self.month,
            self.day,
            self.hour,
            self.minute,
            self.second,
            Some(self.offset_minutes),
        )
    }
    fn local_seconds(&self) -> i64 {
        days_from_civil(self.year as i32, self.month as u32, self.day as u32) * 86_400
            + self.hour as i64 * 3_600
            + self.minute as i64 * 60
            + self.second as i64
    }
    fn from_local_seconds(seconds: i64, offset_minutes: i32) -> Self {
        let (year, month, day) = civil_from_days(seconds.div_euclid(86_400));
        let secs = seconds.rem_euclid(86_400);
        Self {
            year: year as u16,
            month: month as u8,
            day: day as u8,
            hour: (secs / 3600) as u8,
            minute: (secs % 3600 / 60) as u8,
            second: (secs % 60) as u8,
            offset_minutes,
        }
    }
}

#[derive(Debug, Clone, Copy)]
struct ClockSync {
    uptime_ms: u32,
    local_seconds: i64,
    offset_minutes: i32,
}

#[derive(Debug, Clone, Default)]
pub struct LogClock {
    sync: Option<ClockSync>,
}

impl LogClock {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn is_synced(&self) -> bool {
        self.sync.is_some()
    }
    pub fn sync_from_network(&mut self, uptime_ms: u32, time: NetworkDateTime) {
        self.sync = Some(ClockSync {
            uptime_ms,
            local_seconds: time.local_seconds(),
            offset_minutes: time.offset_minutes,
        });
    }
    pub fn timestamp(&self, uptime_ms: u32) -> String {
        match self.sync {
            Some(sync) => NetworkDateTime::from_local_seconds(
                sync.local_seconds + i64::from(elapsed_since(sync.uptime_ms, uptime_ms) / 1000),
                sync.offset_minutes,
            )
            .format(),
            None => format_unsynced(u64::from(uptime_ms) / 1000),
        }
    }
}

pub fn parse_cclk_time(body: &str) -> Option<NetworkDateTime> {
    let trimmed = body.trim();
    let value = if let Some(start) = trimmed.find('"') {
        let rest = &trimmed[start + 1..];
        &rest[..rest.find('"')?]
    } else {
        trimmed.strip_prefix("+CCLK:")?.trim()
    };
    if value.len() < 20
        || &value[2..3] != "/"
        || &value[5..6] != "/"
        || &value[8..9] != ","
        || &value[11..12] != ":"
        || &value[14..15] != ":"
    {
        return None;
    }
    let sign = &value[17..18];
    let quarters: i32 = value[18..].parse().ok()?;
    let short_year = value[0..2].parse::<u16>().ok()?;
    let time = NetworkDateTime {
        year: if short_year >= 70 {
            1900 + short_year
        } else {
            2000 + short_year
        },
        month: value[3..5].parse().ok()?,
        day: value[6..8].parse().ok()?,
        hour: value[9..11].parse().ok()?,
        minute: value[12..14].parse().ok()?,
        second: value[15..17].parse().ok()?,
        offset_minutes: match sign {
            "+" => quarters * 15,
            "-" => -quarters * 15,
            _ => return None,
        },
    };
    (time.year >= 2024
        && (1..=12).contains(&time.month)
        && (1..=days_in_month(time.year as i32, time.month)).contains(&time.day)
        && time.hour < 24
        && time.minute < 60
        && time.second < 60)
        .then_some(time)
}

fn format_unsynced(seconds: u64) -> String {
    let (year, month, day) = civil_from_days(days_from_civil(0, 1, 1) + (seconds / 86_400) as i64);
    let secs = seconds % 86_400;
    format_datetime(
        year,
        month as u8,
        day as u8,
        (secs / 3600) as u8,
        (secs % 3600 / 60) as u8,
        (secs % 60) as u8,
        None,
    )
}

fn format_datetime(
    year: i32,
    month: u8,
    day: u8,
    hour: u8,
    minute: u8,
    second: u8,
    offset: Option<i32>,
) -> String {
    let mut out = format!("{year:04}-{month:02}-{day:02} {hour:02}:{minute:02}:{second:02}");
    if let Some(offset) = offset {
        let sign = if offset < 0 { '-' } else { '+' };
        let abs = offset.unsigned_abs();
        out.push_str(&format!("{sign}{:02}:{:02}", abs / 60, abs % 60));
    }
    out
}

fn days_in_month(year: i32, month: u8) -> u8 {
    match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if (year % 4 == 0 && year % 100 != 0) || year % 400 == 0 => 29,
        2 => 28,
        _ => 0,
    }
}
fn div_floor(a: i32, b: i32) -> i32 {
    let q = a / b;
    let r = a % b;
    if r != 0 && ((r > 0) != (b > 0)) {
        q - 1
    } else {
        q
    }
}
fn days_from_civil(year: i32, month: u32, day: u32) -> i64 {
    let year = year - i32::from(month <= 2);
    let era = div_floor(year, 400);
    let yoe = year - era * 400;
    let month = month as i32;
    let doy = (153 * (month + if month > 2 { -3 } else { 9 }) + 2) / 5 + day as i32 - 1;
    (era * 146_097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719_468) as i64
}
fn civil_from_days(days: i64) -> (i32, u32, u32) {
    let days = days + 719_468;
    let era = if days >= 0 { days } else { days - 146_096 } / 146_097;
    let doe = days - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let year = yoe as i32 + era as i32 * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = mp + if mp < 10 { 3 } else { -9 };
    (year + i32::from(month <= 2), month as u32, day as u32)
}

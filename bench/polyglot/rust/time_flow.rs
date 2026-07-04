// Civil-calendar arithmetic ported from the C++ row (Howard Hinnant's
// days_from_civil/civil_from_days algorithms); Rust std has no calendar API.

const NANOS_PER_SECOND: i64 = 1_000_000_000;
const SECONDS_PER_DAY: i64 = 86_400;

#[derive(Clone, Copy)]
struct CivilDate {
    year: i64,
    month: i32,
    day: i32,
}

#[derive(Clone, Copy)]
struct Fields {
    date: CivilDate,
    hour: i32,
    minute: i32,
    second: i32,
    nanosecond: i32,
}

#[derive(Clone, Copy)]
struct Instant {
    seconds: i64,
    nanosecond: i32,
}

#[derive(Clone, Copy, Default)]
struct Period {
    months: i64,
    days: i64,
    nanoseconds: i64,
}

fn floor_div(a: i64, b: i64) -> i64 {
    let q = a / b;
    let r = a % b;
    if r != 0 && ((r < 0) != (b < 0)) {
        q - 1
    } else {
        q
    }
}

fn floor_mod(a: i64, b: i64) -> i64 {
    a - floor_div(a, b) * b
}

fn days_from_civil(mut y: i64, m: i32, d: i32) -> i64 {
    if m <= 2 {
        y -= 1;
    }
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = (y - era * 400) as u64;
    let mp = (m + if m > 2 { -3 } else { 9 }) as u64;
    let doy = (153 * mp + 2) / 5 + d as u64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe as i64 - 719_468
}

fn civil_from_days(mut z: i64) -> CivilDate {
    z += 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let mut y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as i32;
    let m = mp as i32 + if mp < 10 { 3 } else { -9 };
    if m <= 2 {
        y += 1;
    }
    CivilDate { year: y, month: m, day: d }
}

fn leap_year(year: i64) -> bool {
    year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
}

fn days_in_month(year: i64, month: i32) -> i32 {
    const DAYS: [i32; 12] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if month == 2 && leap_year(year) {
        29
    } else {
        DAYS[(month - 1) as usize]
    }
}

impl Instant {
    fn utc(
        year: i64,
        month: i32,
        day: i32,
        hour: i32,
        minute: i32,
        second: i32,
        nanosecond: i32,
    ) -> Instant {
        Instant {
            seconds: days_from_civil(year, month, day) * SECONDS_PER_DAY
                + (hour as i64) * 3600
                + (minute as i64) * 60
                + second as i64,
            nanosecond,
        }
    }

    fn from_unix_ms(milliseconds: i64) -> Instant {
        Instant {
            seconds: floor_div(milliseconds, 1000),
            nanosecond: (floor_mod(milliseconds, 1000) * 1_000_000) as i32,
        }
    }

    fn from_unix_ns(nanoseconds: i64) -> Instant {
        Instant {
            seconds: floor_div(nanoseconds, NANOS_PER_SECOND),
            nanosecond: floor_mod(nanoseconds, NANOS_PER_SECOND) as i32,
        }
    }

    fn parse_iso8601(text: &str) -> Instant {
        let year: i64 = text[0..4].parse().expect("year");
        let month: i32 = text[5..7].parse().expect("month");
        let day: i32 = text[8..10].parse().expect("day");
        let hour: i32 = text[11..13].parse().expect("hour");
        let minute: i32 = text[14..16].parse().expect("minute");
        let second: i32 = text[17..19].parse().expect("second");
        let offset_pos = text.len() - 6;
        let mut fraction = text[20..offset_pos].to_string();
        while fraction.len() < 9 {
            fraction.push('0');
        }
        let nanosecond: i32 = fraction[0..9].parse().expect("nanosecond");
        let sign: i64 = if text.as_bytes()[offset_pos] == b'-' { -1 } else { 1 };
        let offset_hours: i64 =
            text[offset_pos + 1..offset_pos + 3].parse().expect("offset hours");
        let offset_minutes: i64 =
            text[offset_pos + 4..offset_pos + 6].parse().expect("offset minutes");
        let offset_seconds = sign * (offset_hours * 3600 + offset_minutes * 60);
        let mut local = Instant::utc(year, month, day, hour, minute, second, nanosecond);
        local.seconds -= offset_seconds;
        local
    }

    fn fields(&self) -> Fields {
        let days = floor_div(self.seconds, SECONDS_PER_DAY);
        let second_of_day = floor_mod(self.seconds, SECONDS_PER_DAY) as i32;
        Fields {
            date: civil_from_days(days),
            hour: second_of_day / 3600,
            minute: (second_of_day / 60) % 60,
            second: second_of_day % 60,
            nanosecond: self.nanosecond,
        }
    }

    fn iso8601(&self) -> String {
        let f = self.fields();
        let mut out = format!(
            "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}",
            f.date.year, f.date.month, f.date.day, f.hour, f.minute, f.second
        );
        if self.nanosecond != 0 {
            let mut fraction = format!("{:09}", self.nanosecond);
            while fraction.ends_with('0') {
                fraction.pop();
            }
            out.push('.');
            out.push_str(&fraction);
        }
        out.push('Z');
        out
    }

    fn unix_nanoseconds(&self) -> i64 {
        self.seconds * NANOS_PER_SECOND + self.nanosecond as i64
    }

    fn add_period(&self, period: &Period) -> Instant {
        let f = self.fields();
        let total_month = f.date.year * 12 + (f.date.month - 1) as i64 + period.months;
        let year = floor_div(total_month, 12);
        let month = (total_month - year * 12) as i32 + 1;
        let day = f.date.day.min(days_in_month(year, month));
        let base_days = days_from_civil(year, month, day) + period.days;
        let total_nanos = f.nanosecond as i64 + period.nanoseconds;
        let second_delta = floor_div(total_nanos, NANOS_PER_SECOND);
        let nanos = floor_mod(total_nanos, NANOS_PER_SECOND) as i32;
        Instant {
            seconds: base_days * SECONDS_PER_DAY
                + (f.hour as i64) * 3600
                + (f.minute as i64) * 60
                + f.second as i64
                + second_delta,
            nanosecond: nanos,
        }
    }

    fn sub(&self, other: &Instant) -> Period {
        Period {
            months: 0,
            days: 0,
            nanoseconds: self.unix_nanoseconds() - other.unix_nanoseconds(),
        }
    }

    fn gt(&self, other: &Instant) -> bool {
        self.seconds > other.seconds
            || (self.seconds == other.seconds && self.nanosecond > other.nanosecond)
    }
}

impl Period {
    fn plus(&self, other: &Period) -> Period {
        Period {
            months: self.months + other.months,
            days: self.days + other.days,
            nanoseconds: self.nanoseconds + other.nanoseconds,
        }
    }

    fn total_nanoseconds(&self) -> i64 {
        self.days * SECONDS_PER_DAY * NANOS_PER_SECOND + self.nanoseconds
    }
}

fn months(value: i64) -> Period {
    Period { months: value, ..Default::default() }
}

fn days(value: i64) -> Period {
    Period { days: value, ..Default::default() }
}

fn seconds(value: i64) -> Period {
    Period { nanoseconds: value * NANOS_PER_SECOND, ..Default::default() }
}

fn nanoseconds(value: i64) -> Period {
    Period { nanoseconds: value, ..Default::default() }
}

fn positive_mod(value: i64, modulus: i64) -> i64 {
    let result = value % modulus;
    if result < 0 {
        result + modulus
    } else {
        result
    }
}

fn main() {
    let mut checksum: i64 = 0;
    let anchor = Instant::parse_iso8601("2026-06-17T03:04:05.123456789+03:00");

    for i in 0..12000i64 {
        let base = Instant::utc(
            2020 + (i % 7),
            (1 + (i % 12)) as i32,
            (25 + (i % 4)) as i32,
            (i % 24) as i32,
            ((i * 7) % 60) as i32,
            ((i * 11) % 60) as i32,
            ((i * 1_234_567) % 1_000_000_000) as i32,
        );
        let period = months((i % 15) + 1)
            .plus(&days(i % 21))
            .plus(&seconds(i % 3600))
            .plus(&nanoseconds((i % 1000) * 1000));
        let shifted = base.add_period(&period);
        let elapsed = shifted.sub(&base);
        let epoch = Instant::from_unix_ms(1_700_000_000_000 + i * 37);
        let tiny = Instant::from_unix_ns(1_000_000_000 + i * 1_000_003 + 999);
        let clamp = Instant::utc(2024, 1, 31, 0, 0, 0, 0).add_period(&months((i % 3) + 1));
        let probe = anchor.add_period(&seconds(5).plus(&days(1)));
        let shifted_fields = shifted.fields();
        let epoch_fields = epoch.fields();
        let clamp_fields = clamp.fields();
        let probe_fields = probe.fields();

        checksum += shifted_fields.date.year * 37;
        checksum += shifted_fields.date.month as i64 * 31;
        checksum += shifted_fields.date.day as i64 * 29;
        checksum += shifted_fields.hour as i64 * 23;
        checksum += shifted_fields.minute as i64 * 19;
        checksum += shifted_fields.second as i64 * 17;
        checksum += (shifted.nanosecond as i64) % 1_000_003;
        checksum += positive_mod(elapsed.total_nanoseconds(), 1_000_003);
        checksum += epoch_fields.second as i64 + (epoch.nanosecond as i64) % 1009;
        checksum += tiny.unix_nanoseconds() % 9973;
        checksum += clamp_fields.date.day as i64 * 13 + clamp_fields.date.month as i64;
        checksum += probe_fields.date.day as i64 + probe_fields.second as i64;
        if i % 97 == 0 {
            checksum += shifted.iso8601().len() as i64;
        }
        checksum += if shifted.gt(&base) { 7 } else { 3 };
        checksum %= 2147483647;
    }

    println!("{}", checksum);
}

#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone


NANOS_PER_SECOND = 1_000_000_000


@dataclass(frozen=True)
class Period:
    months: int = 0
    days: int = 0
    nanoseconds: int = 0

    def __add__(self, other: Period | Instant) -> Period | Instant:
        if isinstance(other, Instant):
            return other + self
        return Period(
            self.months + other.months,
            self.days + other.days,
            self.nanoseconds + other.nanoseconds,
        )

    def total_nanoseconds(self) -> int:
        if self.months != 0:
            raise ValueError("calendar period has no fixed total")
        return self.days * 86_400 * NANOS_PER_SECOND + self.nanoseconds


@dataclass(frozen=True)
class Instant:
    seconds: int
    nanosecond: int = 0

    @staticmethod
    def utc(
        year: int,
        month: int,
        day: int,
        hour: int = 0,
        minute: int = 0,
        second: int = 0,
        nanosecond: int = 0,
    ) -> Instant:
        dt = datetime(year, month, day, hour, minute, second, tzinfo=timezone.utc)
        return Instant(int(dt.timestamp()), nanosecond)

    @staticmethod
    def from_unix_ms(milliseconds: int) -> Instant:
        seconds = milliseconds // 1000
        nanos = (milliseconds - seconds * 1000) * 1_000_000
        return Instant(seconds, nanos)

    @staticmethod
    def from_unix_ns(nanoseconds: int) -> Instant:
        seconds = nanoseconds // NANOS_PER_SECOND
        nanos = nanoseconds - seconds * NANOS_PER_SECOND
        return Instant(seconds, nanos)

    @staticmethod
    def parse_iso8601(text: str) -> Instant:
        prefix, offset = text[:-6], text[-6:]
        sign = 1 if offset[0] == "+" else -1
        offset_seconds = sign * (int(offset[1:3]) * 3600 + int(offset[4:6]) * 60)
        head, frac = prefix.split(".")
        dt = datetime.strptime(head, "%Y-%m-%dT%H:%M:%S").replace(
            tzinfo=timezone.utc
        )
        frac = (frac + "000000000")[:9]
        return Instant(int(dt.timestamp()) - offset_seconds, int(frac))

    def fields(self) -> datetime:
        return datetime.fromtimestamp(self.seconds, tz=timezone.utc)

    def year(self) -> int:
        return self.fields().year

    def month(self) -> int:
        return self.fields().month

    def day(self) -> int:
        return self.fields().day

    def hour(self) -> int:
        return self.fields().hour

    def minute(self) -> int:
        return self.fields().minute

    def second(self) -> int:
        return self.fields().second

    def iso8601(self) -> str:
        dt = self.fields()
        text = dt.strftime("%Y-%m-%dT%H:%M:%S")
        if self.nanosecond != 0:
            frac = f"{self.nanosecond:09d}".rstrip("0")
            text += f".{frac}"
        return text + "Z"

    def unix_nanoseconds(self) -> int:
        return self.seconds * NANOS_PER_SECOND + self.nanosecond

    def __add__(self, period: Period) -> Instant:
        dt = self.fields()
        total_month = dt.year * 12 + (dt.month - 1) + period.months
        year, month0 = divmod(total_month, 12)
        month = month0 + 1
        day = min(dt.day, days_in_month(year, month))
        shifted = datetime(
            year,
            month,
            day,
            dt.hour,
            dt.minute,
            dt.second,
            tzinfo=timezone.utc,
        )
        shifted += timedelta(days=period.days)
        total_nanos = self.nanosecond + period.nanoseconds
        seconds_delta, nanos = divmod(total_nanos, NANOS_PER_SECOND)
        shifted += timedelta(seconds=seconds_delta)
        return Instant(int(shifted.timestamp()), nanos)

    def __sub__(self, other: Instant | Period) -> Period | Instant:
        if isinstance(other, Period):
            return self + Period(-other.months, -other.days, -other.nanoseconds)
        return Period(nanoseconds=self.unix_nanoseconds() - other.unix_nanoseconds())

    def __gt__(self, other: Instant) -> bool:
        return (self.seconds, self.nanosecond) > (other.seconds, other.nanosecond)


def days_in_month(year: int, month: int) -> int:
    if month == 2:
        leap = year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)
        return 29 if leap else 28
    return [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1]


def months(value: int) -> Period:
    return Period(months=value)


def days(value: int) -> Period:
    return Period(days=value)


def seconds(value: int) -> Period:
    return Period(nanoseconds=value * NANOS_PER_SECOND)


def nanoseconds(value: int) -> Period:
    return Period(nanoseconds=value)


def main() -> int:
    checksum = 0
    anchor = Instant.parse_iso8601("2026-06-17T03:04:05.123456789+03:00")

    for i in range(12000):
        base = Instant.utc(
            2020 + (i % 7),
            1 + (i % 12),
            25 + (i % 4),
            hour=i % 24,
            minute=(i * 7) % 60,
            second=(i * 11) % 60,
            nanosecond=(i * 1_234_567) % 1_000_000_000,
        )
        period = months((i % 15) + 1)
        period = period + days(i % 21)
        period = period + seconds(i % 3600)
        period = period + nanoseconds((i % 1000) * 1000)
        shifted = base + period
        elapsed = shifted - base
        epoch = Instant.from_unix_ms(1_700_000_000_000 + (i * 37))
        tiny = Instant.from_unix_ns(1_000_000_000 + (i * 1_000_003) + 999)
        clamp = Instant.utc(2024, 1, 31) + months((i % 3) + 1)
        probe = seconds(5) + days(1) + anchor

        checksum += shifted.year() * 37
        checksum += shifted.month() * 31
        checksum += shifted.day() * 29
        checksum += shifted.hour() * 23
        checksum += shifted.minute() * 19
        checksum += shifted.second() * 17
        checksum += shifted.nanosecond % 1_000_003
        checksum += elapsed.total_nanoseconds() % 1_000_003
        checksum += epoch.second() + epoch.nanosecond % 1009
        checksum += tiny.unix_nanoseconds() % 9973
        checksum += clamp.day() * 13 + clamp.month()
        checksum += probe.day() + probe.second()
        if i % 97 == 0:
            checksum += len(shifted.iso8601())
        checksum += 7 if shifted > base else 3
        checksum %= 2_147_483_647

    return checksum


if __name__ == "__main__":
    print(main())

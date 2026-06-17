package main

import (
	"fmt"
	"strconv"
	"strings"
	"time"
)

const nanosPerSecond int64 = 1_000_000_000

type Period struct {
	months      int64
	days        int64
	nanoseconds int64
}

type Instant struct {
	seconds    int64
	nanosecond int64
}

func periodAdd(left Period, right Period) Period {
	return Period{
		months:      left.months + right.months,
		days:        left.days + right.days,
		nanoseconds: left.nanoseconds + right.nanoseconds,
	}
}

func months(value int64) Period {
	return Period{months: value}
}

func days(value int64) Period {
	return Period{days: value}
}

func seconds(value int64) Period {
	return Period{nanoseconds: value * nanosPerSecond}
}

func nanoseconds(value int64) Period {
	return Period{nanoseconds: value}
}

func (period Period) totalNanoseconds() int64 {
	return period.days*86_400*nanosPerSecond + period.nanoseconds
}

func utc(year int64, month int, day int, hour int, minute int, second int, nanosecond int64) Instant {
	value := time.Date(int(year), time.Month(month), day, hour, minute, second, 0, time.UTC)
	return Instant{seconds: value.Unix(), nanosecond: nanosecond}
}

func fromUnixMs(milliseconds int64) Instant {
	seconds := floorDiv(milliseconds, 1000)
	millis := milliseconds - seconds*1000
	return Instant{seconds: seconds, nanosecond: millis * 1_000_000}
}

func fromUnixNs(nanos int64) Instant {
	seconds := floorDiv(nanos, nanosPerSecond)
	rest := nanos - seconds*nanosPerSecond
	return Instant{seconds: seconds, nanosecond: rest}
}

func parseIso8601(text string) Instant {
	offsetPos := len(text) - 6
	year, _ := strconv.ParseInt(text[0:4], 10, 64)
	month, _ := strconv.Atoi(text[5:7])
	day, _ := strconv.Atoi(text[8:10])
	hour, _ := strconv.Atoi(text[11:13])
	minute, _ := strconv.Atoi(text[14:16])
	second, _ := strconv.Atoi(text[17:19])
	fraction := text[20:offsetPos]
	if len(fraction) < 9 {
		fraction += strings.Repeat("0", 9-len(fraction))
	}
	nanos, _ := strconv.ParseInt(fraction[0:9], 10, 64)
	sign := int64(1)
	if text[offsetPos] == '-' {
		sign = -1
	}
	offsetHour, _ := strconv.ParseInt(text[offsetPos+1:offsetPos+3], 10, 64)
	offsetMinute, _ := strconv.ParseInt(text[offsetPos+4:offsetPos+6], 10, 64)
	offsetSeconds := sign * (offsetHour*3600 + offsetMinute*60)
	local := utc(year, month, day, hour, minute, second, nanos)
	local.seconds -= offsetSeconds
	return local
}

func (instant Instant) timeValue() time.Time {
	return time.Unix(instant.seconds, 0).UTC()
}

func (instant Instant) year() int64 {
	return int64(instant.timeValue().Year())
}

func (instant Instant) month() int64 {
	return int64(instant.timeValue().Month())
}

func (instant Instant) day() int64 {
	return int64(instant.timeValue().Day())
}

func (instant Instant) hour() int64 {
	return int64(instant.timeValue().Hour())
}

func (instant Instant) minute() int64 {
	return int64(instant.timeValue().Minute())
}

func (instant Instant) second() int64 {
	return int64(instant.timeValue().Second())
}

func (instant Instant) iso8601() string {
	text := instant.timeValue().Format("2006-01-02T15:04:05")
	if instant.nanosecond != 0 {
		fraction := fmt.Sprintf("%09d", instant.nanosecond)
		fraction = strings.TrimRight(fraction, "0")
		text += "." + fraction
	}
	return text + "Z"
}

func (instant Instant) unixNanoseconds() int64 {
	return instant.seconds*nanosPerSecond + instant.nanosecond
}

func addPeriod(instant Instant, period Period) Instant {
	base := instant.timeValue()
	totalMonth := int64(base.Year())*12 + int64(base.Month()-1) + period.months
	year := floorDiv(totalMonth, 12)
	month0 := totalMonth - year*12
	month := int(month0 + 1)
	day := base.Day()
	if maxDay := daysInMonth(year, month); day > maxDay {
		day = maxDay
	}
	shifted := time.Date(int(year), time.Month(month), day, base.Hour(), base.Minute(), base.Second(), 0, time.UTC)
	shifted = shifted.Add(time.Duration(period.days) * 24 * time.Hour)
	totalNanos := instant.nanosecond + period.nanoseconds
	secondsDelta := floorDiv(totalNanos, nanosPerSecond)
	nanos := totalNanos - secondsDelta*nanosPerSecond
	shifted = shifted.Add(time.Duration(secondsDelta) * time.Second)
	return Instant{seconds: shifted.Unix(), nanosecond: nanos}
}

func difference(left Instant, right Instant) Period {
	return Period{nanoseconds: left.unixNanoseconds() - right.unixNanoseconds()}
}

func greaterThan(left Instant, right Instant) bool {
	return left.seconds > right.seconds || (left.seconds == right.seconds && left.nanosecond > right.nanosecond)
}

func daysInMonth(year int64, month int) int {
	if month == 2 {
		if year%4 == 0 && (year%100 != 0 || year%400 == 0) {
			return 29
		}
		return 28
	}
	return []int{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}[month-1]
}

func floorDiv(a int64, b int64) int64 {
	q := a / b
	r := a % b
	if r != 0 && ((r < 0) != (b < 0)) {
		q--
	}
	return q
}

func positiveMod(value int64, modulus int64) int64 {
	result := value % modulus
	if result < 0 {
		return result + modulus
	}
	return result
}

func main() {
	checksum := int64(0)
	anchor := parseIso8601("2026-06-17T03:04:05.123456789+03:00")

	for i := int64(0); i < 12000; i++ {
		base := utc(
			2020+(i%7),
			int(1+(i%12)),
			int(25+(i%4)),
			int(i%24),
			int((i*7)%60),
			int((i*11)%60),
			(i*1_234_567)%1_000_000_000,
		)
		period := months((i % 15) + 1)
		period = periodAdd(period, days(i%21))
		period = periodAdd(period, seconds(i%3600))
		period = periodAdd(period, nanoseconds((i%1000)*1000))
		shifted := addPeriod(base, period)
		elapsed := difference(shifted, base)
		epoch := fromUnixMs(1_700_000_000_000 + (i * 37))
		tiny := fromUnixNs(1_000_000_000 + (i * 1_000_003) + 999)
		clamp := addPeriod(utc(2024, 1, 31, 0, 0, 0, 0), months((i%3)+1))
		probe := addPeriod(addPeriod(anchor, days(1)), seconds(5))

		checksum += shifted.year() * 37
		checksum += shifted.month() * 31
		checksum += shifted.day() * 29
		checksum += shifted.hour() * 23
		checksum += shifted.minute() * 19
		checksum += shifted.second() * 17
		checksum += shifted.nanosecond % 1_000_003
		checksum += positiveMod(elapsed.totalNanoseconds(), 1_000_003)
		checksum += epoch.second() + epoch.nanosecond%1009
		checksum += tiny.unixNanoseconds() % 9973
		checksum += clamp.day()*13 + clamp.month()
		checksum += probe.day() + probe.second()
		if i%97 == 0 {
			checksum += int64(len(shifted.iso8601()))
		}
		if greaterThan(shifted, base) {
			checksum += 7
		} else {
			checksum += 3
		}
		checksum %= 2_147_483_647
	}

	fmt.Println(checksum)
}

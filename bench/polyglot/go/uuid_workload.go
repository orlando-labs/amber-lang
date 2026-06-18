package main

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"strings"
	"time"
)

const rounds = 5000
const fixedText = "550e8400-e29b-41d4-a716-446655440000"

type UUID [16]byte

func randomUUID() UUID {
	var out UUID
	if _, err := rand.Read(out[:]); err != nil {
		panic(err)
	}
	return out
}

func uuidV4() UUID {
	out := randomUUID()
	out[6] = (out[6] & 0x0f) | 0x40
	out[8] = (out[8] & 0x3f) | 0x80
	return out
}

func uuidV7() UUID {
	out := randomUUID()
	milliseconds := uint64(time.Now().UnixMilli())
	for index := 0; index < 6; index++ {
		out[index] = byte(milliseconds >> ((5 - index) * 8))
	}
	out[6] = (out[6] & 0x0f) | 0x70
	out[8] = (out[8] & 0x3f) | 0x80
	return out
}

func parseUUID(text string) UUID {
	normalized := strings.ReplaceAll(text, "-", "")
	bytes, err := hex.DecodeString(normalized)
	if err != nil || len(bytes) != 16 {
		panic("invalid UUID")
	}
	var out UUID
	copy(out[:], bytes)
	return out
}

func (value UUID) String() string {
	text := hex.EncodeToString(value[:])
	return fmt.Sprintf("%s-%s-%s-%s-%s",
		text[0:8], text[8:12], text[12:16], text[16:20], text[20:32])
}

func (value UUID) Version() int64 {
	return int64((value[6] >> 4) & 0x0f)
}

func main() {
	checksum := int64(0)
	for i := 0; i < rounds; i++ {
		v4 := uuidV4()
		v4Text := v4.String()
		parsedV4 := parseUUID(v4Text)

		v7 := uuidV7()
		v7Text := v7.String()
		parsedV7 := parseUUID(v7Text)

		delegated := uuidV4()
		delegatedJSON := fmt.Sprintf("\"%s\"", delegated.String())

		fixed := parseUUID(strings.ToUpper(fixedText))
		fixedCanonical := fixed.String()

		checksum += v4.Version()
		checksum += v7.Version()
		checksum += delegated.Version()
		checksum += int64(len(v4Text))
		checksum += int64(len(v7Text))
		checksum += int64(len(delegatedJSON))

		if v4 == parsedV4 {
			checksum += 11
		}
		if v7 == parsedV7 {
			checksum += 13
		}
		checksum += 17
		if fixed.Version() == 4 && fixedCanonical == fixedText {
			checksum += 19
		}
		if strings.Contains(v4Text, "-") && strings.Contains(v7Text, "-") {
			checksum += 23
		}
	}
	fmt.Println(checksum)
}

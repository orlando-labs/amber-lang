package main

import (
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"math/big"
	"strings"
)

const rounds = 2000

func randomBytes(count int) []byte {
	out := make([]byte, count)
	if _, err := rand.Read(out); err != nil {
		panic(err)
	}
	return out
}

func randomInt(min int64, max int64) int64 {
	value, err := rand.Int(rand.Reader, big.NewInt(max-min+1))
	if err != nil {
		panic(err)
	}
	return min + value.Int64()
}

func uuidV4() string {
	bytes := randomBytes(16)
	bytes[6] = (bytes[6] & 0x0f) | 0x40
	bytes[8] = (bytes[8] & 0x3f) | 0x80
	text := hex.EncodeToString(bytes)
	return fmt.Sprintf("%s-%s-%s-%s-%s",
		text[0:8], text[8:12], text[12:16], text[16:20], text[20:32])
}

func main() {
	checksum := int64(0)
	for i := 0; i < rounds; i++ {
		checksum += int64(len(randomBytes(32)))

		hx := hex.EncodeToString(randomBytes(16))
		decodedHex, _ := hex.DecodeString(hx)
		checksum += int64(len(decodedHex))

		b64 := base64.StdEncoding.EncodeToString(randomBytes(18))
		decodedB64, _ := base64.StdEncoding.DecodeString(b64)
		checksum += int64(len(decodedB64))

		compact := base64.RawStdEncoding.EncodeToString(randomBytes(17))
		decodedCompact, _ := base64.RawStdEncoding.DecodeString(compact)
		checksum += int64(len(decodedCompact))

		url := base64.RawURLEncoding.EncodeToString(randomBytes(18))
		decodedURL, _ := base64.RawURLEncoding.DecodeString(url)
		checksum += int64(len(decodedURL))

		id := uuidV4()
		if len(id) == 36 && strings.Contains(id, "-") {
			checksum += 37
		}

		value := randomInt(100, 999)
		if value >= 100 && value <= 999 {
			checksum += 10
		}
	}
	fmt.Println(checksum)
}

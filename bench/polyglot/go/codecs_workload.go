package main

import (
	"encoding/base64"
	"encoding/hex"
	"fmt"
)

func main() {
	checksum := int64(0)
	for i := 0; i < 5000; i++ {
		raw := []byte(fmt.Sprintf("event-%d:%d", i, (i*17)%100000))

		b64 := base64.StdEncoding.EncodeToString(raw)
		roundtrip, _ := base64.StdEncoding.DecodeString(b64)
		checksum += int64(len(roundtrip))
		checksum += int64(roundtrip[0]) + int64(roundtrip[len(roundtrip)-1])

		hx := hex.EncodeToString(raw)
		hexBytes, _ := hex.DecodeString(hx)
		url := base64.RawURLEncoding.EncodeToString(hexBytes)
		back, _ := base64.RawURLEncoding.DecodeString(url)
		checksum += int64(len(back))
		checksum += int64(back[1])

		decoded, _ := hex.DecodeString(hex.EncodeToString(back))
		checksum += int64(decoded[2])

		if i%16 == 0 {
			folded, _ := base64.StdEncoding.DecodeString(b64 + "\n")
			checksum += int64(len(folded))
		}
		if i%31 == 0 {
			padded := base64.URLEncoding.EncodeToString(raw)
			paddedBytes, _ := base64.URLEncoding.DecodeString(padded)
			checksum += int64(len(paddedBytes))
		}
		if i%17 == 0 {
			decodedHex, _ := hex.DecodeString(hx)
			checksum += int64(decodedHex[0])
		}
	}
	fmt.Println(checksum)
}

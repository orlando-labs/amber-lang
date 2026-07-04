package main

import (
	"fmt"
	"strconv"
	"strings"
)

func mainWorkload() int64 {
	var checksum int64
	for i := 0; i < 4000; i++ {
		token := "user-" + strconv.Itoa(i) + "-record"
		upper := strings.ToUpper(token)
		lower := strings.ToLower(upper)
		line := lower + "|" + token + "|segment-" + strconv.Itoa(i%97)
		parts := strings.Split(line, "|")
		rebuilt := parts[0] + ";" + parts[1] + ";" + parts[2]
		replaced := strings.ReplaceAll(rebuilt, "user-", "member-")
		padded := "  " + replaced + "  "
		trimmed := strings.TrimSpace(padded)
		checksum += int64(len(trimmed)) + int64(len(parts))
		if strings.Contains(trimmed, "member-") {
			checksum += 3
		}
		if strings.HasPrefix(trimmed, "member-") {
			checksum += 7
		}
		if strings.HasSuffix(trimmed, "7") {
			checksum += 11
		}
		if i%5 == 0 {
			checksum += int64(len(strings.ReplaceAll(line, "e", "E")))
		}
	}
	return checksum
}

func main() {
	fmt.Println(mainWorkload())
}

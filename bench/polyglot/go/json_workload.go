package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
)

const jsonPath = "bench/polyglot/build/json/events.jsonl"

type doc struct {
	ID    int   `json:"id"`
	Value int   `json:"value"`
	Group int   `json:"group"`
	Items []int `json:"items"`
	Name  string `json:"name"`
}

type row struct {
	ID    int    `json:"id"`
	Value int    `json:"value"`
	Group int    `json:"group"`
	Name  string `json:"name"`
}

func main() {
	checksum := int64(0)
	for i := 0; i < 1000; i++ {
		document := doc{
			ID:    i,
			Value: i * 2,
			Group: i % 7,
			Items: []int{i, i + 1, i + 2},
			Name:  "event",
		}
		compact, err := json.Marshal(document)
		if err != nil {
			panic(err)
		}
		var parsed doc
		if err := json.Unmarshal(compact, &parsed); err != nil {
			panic(err)
		}
		checksum += int64(parsed.ID + parsed.Value + parsed.Group)
		checksum += int64(parsed.Items[1])
		if i < 20 {
			pretty, err := json.MarshalIndent(parsed, "", "  ")
			if err != nil {
				panic(err)
			}
			var reparsed doc
			if err := json.Unmarshal(pretty, &reparsed); err != nil {
				panic(err)
			}
			checksum += int64(reparsed.Items[2])
		}
	}

	file, err := os.Open(jsonPath)
	if err != nil {
		panic(err)
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	count := int64(0)
	for scanner.Scan() {
		var parsed row
		if err := json.Unmarshal(scanner.Bytes(), &parsed); err != nil {
			panic(err)
		}
		checksum += int64(parsed.ID*3 + parsed.Value - parsed.Group)
		count++
	}
	if err := scanner.Err(); err != nil {
		panic(err)
	}

	fmt.Println(checksum + count)
}

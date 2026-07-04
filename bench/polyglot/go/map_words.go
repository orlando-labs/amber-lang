package main

import (
	"fmt"
	"strconv"
)

func mainWorkload() int64 {
	m := make(map[string]int64)
	var i int64
	for i < 30000 {
		key := "w" + strconv.FormatInt((i*i+i/3)%2000, 10)
		if _, ok := m[key]; ok {
			m[key] = m[key] + 1
		} else {
			m[key] = 1
		}
		i++
	}
	var checksum int64
	for k, v := range m {
		checksum += v * int64(len(k))
	}
	var hits int64
	var j int64
	for j < 10000 {
		probe := "w" + strconv.FormatInt((j*7)%3000, 10)
		if value, ok := m[probe]; ok {
			hits += value
		}
		j++
	}
	return checksum + hits + int64(len(m))
}

func main() {
	fmt.Println(mainWorkload())
}

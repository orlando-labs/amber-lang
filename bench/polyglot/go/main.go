package main

import "fmt"

const (
	iterations int64 = 1000000
	limit      int64 = 2147483647
)

func mainWorkload() int64 {
	var i int64 = 0
	var a int64 = 1
	var b int64 = 2
	var checksum int64 = 0
	for i < iterations {
		a = a + b + 3
		if a > limit {
			a = a - limit
		}
		b = b + a + i
		if b > limit {
			b = b - limit
		}
		if a > b {
			checksum = checksum + a - b
		} else {
			checksum = checksum + b - a
		}
		i = i + 1
	}
	return checksum + a + b
}

func main() {
	fmt.Println(mainWorkload())
}

package main

import "fmt"

const (
	callLimit  int64 = 2147483647
	callRounds int64 = 20000
)

type row [3]int64
type weights [3]int64

func wrap(value int64, limit int64) int64 {
	for value > limit {
		value = value - limit
	}
	return value
}

func laneIndex(index int64, size int64) int64 {
	return index - (index/size)*size
}

func mix(value int64, salt int64) int64 {
	mixed := value + salt + 17
	mixed = mixed * 13
	return wrap(mixed, callLimit)
}

func pick(values weights, index int64) int64 {
	return values[index]
}

func scoreRow(values row, valuesWeights weights, bias int64) int64 {
	x := values[0]
	y := values[1]
	z := values[2]
	mixed := mix(x+pick(valuesWeights, 0), y+bias)
	if mixed > z {
		mixed = mixed - z
	} else {
		mixed = z - mixed
	}
	return mixed + pick(valuesWeights, 1)*y + pick(valuesWeights, 2)
}

func foldRows(rows []row, valuesWeights weights, rounds int64) int64 {
	var total int64 = 0
	var i int64 = 0
	size := int64(len(rows))
	for i < rounds {
		current := rows[laneIndex(i, size)]
		total = wrap(total+scoreRow(current, valuesWeights, i), callLimit)
		i = i + 1
	}
	return total
}

func countLarge(values []int64, threshold int64) int64 {
	var total int64 = 0
	var i int64 = 0
	size := int64(len(values))
	for i < size {
		if values[i] > threshold {
			total = total + 1
		}
		i = i + 1
	}
	return total
}

func sumValues(values []int64) int64 {
	var total int64 = 0
	var i int64 = 0
	size := int64(len(values))
	for i < size {
		total = total + values[i]
		i = i + 1
	}
	return total
}

func mainWorkload() int64 {
	rows := []row{
		{3, 5, 8},
		{13, 21, 34},
		{55, 89, 144},
		{233, 377, 610},
		{987, 1597, 2584},
		{4181, 6765, 10946},
		{17711, 28657, 46368},
		{75025, 121393, 196418},
	}
	valuesWeights := weights{11, 17, 23}
	derived := []int64{
		scoreRow(rows[0], valuesWeights, valuesWeights[0]),
		scoreRow(rows[1], valuesWeights, valuesWeights[0]),
		scoreRow(rows[2], valuesWeights, valuesWeights[0]),
		scoreRow(rows[3], valuesWeights, valuesWeights[0]),
		scoreRow(rows[4], valuesWeights, valuesWeights[0]),
		scoreRow(rows[5], valuesWeights, valuesWeights[0]),
		scoreRow(rows[6], valuesWeights, valuesWeights[0]),
		scoreRow(rows[7], valuesWeights, valuesWeights[0]),
	}
	selectedCount := countLarge(derived, 1000)
	folded := sumValues(derived)
	return foldRows(rows, valuesWeights, callRounds) + folded + selectedCount
}

func main() {
	fmt.Println(mainWorkload())
}

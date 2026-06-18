package main

import (
	"crypto/hmac"
	"crypto/md5"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"hash/crc32"
)

var payloads = [][]byte{
	[]byte("Amber digest polyglot benchmark payload zero"),
	[]byte("Amber digest polyglot benchmark payload one 1234567890"),
	[]byte("The quick brown fox jumps over the lazy dog"),
	[]byte("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
}

var key = []byte("amber-digest-benchmark-key")

func foldDigest(checksum uint64, digest []byte) uint64 {
	return checksum + uint64(len(digest)) + uint64(digest[0]) +
		uint64(digest[len(digest)-1])
}

func mainWorkload() uint64 {
	var checksum uint64
	var crcBytes [4]byte
	for i := 0; i < 4000; i++ {
		data := payloads[i%len(payloads)]
		binary.BigEndian.PutUint32(crcBytes[:], crc32.ChecksumIEEE(data))
		checksum = foldDigest(checksum, crcBytes[:])

		md5Digest := md5.Sum(data)
		checksum = foldDigest(checksum, md5Digest[:])
		sha1Digest := sha1.Sum(data)
		checksum = foldDigest(checksum, sha1Digest[:])
		sha256Digest := sha256.Sum256(data)
		checksum = foldDigest(checksum, sha256Digest[:])

		mac := hmac.New(sha256.New, key)
		_, _ = mac.Write(data)
		checksum = foldDigest(checksum, mac.Sum(nil))
	}
	return checksum
}

func main() {
	fmt.Println(mainWorkload())
}

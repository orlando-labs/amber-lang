import base64
import binascii


def main():
    checksum = 0
    for i in range(5000):
        raw = f"event-{i}:{(i * 17) % 100000}".encode()

        b64 = base64.b64encode(raw).decode()
        roundtrip = base64.b64decode(b64)
        checksum += len(roundtrip)
        checksum += roundtrip[0] + roundtrip[-1]

        hx = binascii.hexlify(raw).decode()
        url = base64.urlsafe_b64encode(binascii.unhexlify(hx)).decode().rstrip("=")
        back = base64.urlsafe_b64decode(url + "=" * ((4 - len(url) % 4) % 4))
        checksum += len(back)
        checksum += back[1]

        decoded = binascii.unhexlify(binascii.hexlify(back))
        checksum += decoded[2]

        if i % 16 == 0:
            folded = base64.b64decode(b64 + "\n")
            checksum += len(folded)

        if i % 31 == 0:
            padded = base64.urlsafe_b64encode(raw).decode()
            checksum += len(base64.urlsafe_b64decode(padded))

        if i % 17 == 0:
            checksum += binascii.unhexlify(hx)[0]

    print(checksum)


if __name__ == "__main__":
    main()

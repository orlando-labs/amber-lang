import json


JSON_PATH = "bench/polyglot/build/json/events.jsonl"


def main() -> int:
    checksum = 0
    for i in range(1000):
        document = {
            "id": i,
            "value": i * 2,
            "group": i % 7,
            "items": [i, i + 1, i + 2],
            "name": "event",
        }
        compact = json.dumps(document, separators=(",", ":"))
        parsed = json.loads(compact)
        checksum += parsed["id"] + parsed["value"] + parsed["group"]
        checksum += parsed["items"][1]
        if i < 20:
            pretty = json.dumps(parsed, indent=2)
            reparsed = json.loads(pretty)
            checksum += reparsed["items"][2]

    count = 0
    with open(JSON_PATH, encoding="utf-8") as handle:
        for line in handle:
            row = json.loads(line)
            checksum += row["id"] * 3 + row["value"] - row["group"]
            count += 1
    return checksum + count


if __name__ == "__main__":
    print(main())

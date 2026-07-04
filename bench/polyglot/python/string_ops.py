def main_workload():
    checksum = 0
    for i in range(4000):
        token = "user-" + str(i) + "-record"
        upper = token.upper()
        lower = upper.lower()
        line = lower + "|" + token + "|segment-" + str(i % 97)
        parts = line.split("|")
        rebuilt = parts[0] + ";" + parts[1] + ";" + parts[2]
        replaced = rebuilt.replace("user-", "member-")
        padded = "  " + replaced + "  "
        trimmed = padded.strip()
        checksum += len(trimmed) + len(parts)
        if "member-" in trimmed:
            checksum += 3
        if trimmed.startswith("member-"):
            checksum += 7
        if trimmed.endswith("7"):
            checksum += 11
        if i % 5 == 0:
            checksum += len(line.replace("e", "E"))
    return checksum


if __name__ == "__main__":
    print(main_workload())

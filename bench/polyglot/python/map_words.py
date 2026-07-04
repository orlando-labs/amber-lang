def main_workload():
    m = {}
    i = 0
    while i < 30000:
        key = "w" + str((i * i + i // 3) % 2000)
        if key in m:
            m[key] = m[key] + 1
        else:
            m[key] = 1
        i += 1
    checksum = 0
    for k, v in m.items():
        checksum += v * len(k)
    hits = 0
    j = 0
    while j < 10000:
        probe = "w" + str((j * 7) % 3000)
        if probe in m:
            hits += m[probe]
        j += 1
    return checksum + hits + len(m)


if __name__ == "__main__":
    print(main_workload())

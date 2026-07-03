# Amber tzdb snapshot

This directory vendors a compiled IANA time-zone database snapshot for the
`TimeZone` runtime.

- Snapshot version: `2026b`
- Source on the development machine:
  `/var/db/timezone/tz/2026b.1.0/zoneinfo`
- Runtime root: `third_party/tzdb/zoneinfo`

The snapshot contains TZif zone files plus the metadata files distributed with
the source tree, including `+VERSION`, `zone.tab`, `iso3166.tab`, `leapseconds`,
and `tzdata.zi`.

The runtime lookup order is:

1. `AMBER_TZDB_DIR`, when set;
2. executable-relative bundled roots;
3. working-directory-relative bundled roots;
4. host system zoneinfo directories as fallback.

The bundled `tzdata.zi` declares itself public domain. Keep this README updated
when refreshing the snapshot.

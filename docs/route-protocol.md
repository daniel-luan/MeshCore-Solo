# Solo route format and USB protocol, version 1

Implementation: `RouteFormat.h`, `RouteStore.h`, `RouteFilesystem.h`, `RouteProtocol.h` in `examples/companion_radio/`; browser converter and serial client in `tools/gpx-downloader/routes/`. All integers are explicitly little-endian. CRC is standard reflected CRC-32 (polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final complement).

## Disk package

The computer sends the payload without a completion footer. Header is 96 bytes:

| Offset | Field |
| --- | --- |
| 0 | `MCR1` magic (u32) |
| 4, 6 | Version 1 and header length 96 (u16 each) |
| 8 | Total payload bytes (u32) |
| 12,16,20,24 | Point, checkpoint, preview, index-page counts (u32) |
| 28,32,36,40,44 | Offsets of points, checkpoints, labels, previews, index (u32) |
| 48,52,56,60,64 | Root page, distance cm, segment count, label byte count, name label offset (u32) |
| 68,70 | Name length (u16), zero (u16) |
| 72 | Bounding box: min latitude, min longitude, max latitude, max longitude (i32 × 4) |
| 88,92 | CRC of bytes 0–87 (u32), zero (u32) |

Sections must be contiguous and consistent with the header. Coordinates use millionths of degrees. Point records (16 bytes) contain latitude/longitude i32, cumulative distance cm u32, altitude metres i16 (`-32768` = unknown), flags u16 (bit 0 = segment start, others zero). Distances do not increase across segment gaps. Every segment needs at least two points.

Checkpoint records (32 bytes) contain latitude/longitude i32; distance cm, segment number, associated edge's first point index, lateral offset cm, label offset u32; label length u16; zero u16. Labels and route name are UTF-8 bytes, not NUL terminated. Counts and offsets are 32-bit; checked arithmetic rejects overflow.

Preview contains 2–64 point records. Preview flags additionally permit bit 1, marking an elevation gap in the skipped source samples; this breaks the profile line without breaking the geographic path. Index pages are 416 bytes: bounding box at 0, kind u8 at 16 (0 internal, 1 leaf), entry count u8 at 17, zero u16 at 18, first/last point indices u32 at 20/24, zero u32 at 28. Up to 16 entries of 24 bytes begin at 32: bounding box plus reference and extent u32. A leaf has one entry with a first point index and count ≤64. Its bounds include the outgoing edge endpoint. Internal references point backward to child pages, extent zero; children cover consecutive point ranges. Root is the final page and covers all points.

## Activation and capacity

A/B files `/route_a` and `/route_b` live on L1 QSPI LittleFS, otherwise primary filesystem (V4 SPIFFS). Incremental verification checks payload CRC, sections, points, checkpoints, previews, and index relationships before appending a 20-byte footer: `RCMT` magic, payload bytes, generation, payload CRC, footer CRC over its first 16 bytes (all u32). Boot verifies both slots incrementally and selects the newest fully valid generation; a corrupt/incomplete new slot falls back to the old one. Route point/index/preview caches and core state have a compile-time <4096-byte bound.

Capacity reports nominal total, allocated bytes, reserve, reclaimable inactive file bytes, and maximum upload bytes excluding the footer. Reserve is 64 KiB plus two erase blocks on LittleFS; SPIFFS reserves 25% of reported usable filesystem capacity plus 64 KiB. Accounting failure rejects writes. Inactive file size gives a conservative reclaim estimate on block-based filesystems. Actual filesystem overhead can reject a transfer below the reported maximum; the active slot remains intact. Settings use separate checked A/B `/route_cfg_a` and `/route_cfg_b` records, preserving existing preference formats.

## USB framing and requests

Use existing serial transport at 115200 baud: host frame `<` (0x3C), length u16, payload; device `>` (0x3E), length u16, payload. Maximum payload is 176 bytes. BLE commands are rejected, and existing BLE connection priority is retained.

Request header (11 bytes): `F0 53 52 01`, operation u8, request ID u16, session ID u32. Reply header (17 bytes): `7F 53 52 01`, operation, echoed request ID, session, status u8, state u8, next accepted offset u32. Each field follows immediately in this order.

| Op | Request body | Reply body |
| --- | --- | --- |
| 1 CAPABILITIES | Empty | Chunk limit u16 (144); total/allocated/reserve/reclaimable/maximum u32 |
| 2 INFO | Empty | When route exists: bytes/points/distance cm/checkpoints/segments u32; name length u8 and UTF-8 name (≤63 bytes). Otherwise empty. |
| 3 BEGIN | Payload bytes and CRC u32 | Empty; new session and offset in reply header |
| 4 DATA | Offset u32, 1–144 data bytes | Empty; acknowledgement follows completed write |
| 5 STATUS | Empty | CRC scan percentage u32, stored error u8; state reflects subsequent structural verification |
| 6 COMMIT | Empty | Empty; starts incremental verification |
| 7 ABORT | Empty | Empty |
| 8 DELETE | `DELT` confirmation magic u32 | Empty |

Statuses 0–9: OK, unsupported, busy, bad request, no space, I/O error, bad file, bad session, bad offset, USB only. States 0–5: checking, ready, empty, receiving, verifying, failed. INFO can describe the retained active route while a replacement fails. STATUS permits session zero for initial discovery.

One outstanding chunk; reuse request ID and exact bytes for retries. Firmware handles duplicate BEGIN, last accepted DATA, and COMMIT without duplicate appends. Use five-second timeouts with three retries. Upload reception expires after 60 seconds without accepted transfer activity. COMMIT is asynchronous: poll STATUS until ready or failed. A 100% CRC scan does not mean structural validation is finished. Replacement/deletion is rejected while following. Disconnect/cancel affects only the inactive transfer until activation. Import activity does not wake the screen.

### Optional verification diagnostics

Newer STATUS replies retain their original five body bytes and append: marker `0xD1` u8, phase u8, record/cursor u32, checksum scan position u32, expected payload CRC u32, scanned payload CRC u32. Total body length is 23 bytes. Clients must check length and marker before using this optional trailer. Failed verification preserves its phase and cursor. Phases: 1 header, 2 boot footer, 3 checksum, 4 points, 5 checkpoints, 6 preview, 7 index pages, 8 index relationships/bounds, 10 footer write, 11 preview load, 13 footer readback, 14 checkpoint edge association. CRC fields are only directly comparable when the payload scan has completed.

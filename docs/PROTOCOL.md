# BI2X (BIO2) host protocol

Reverse engineered from the 27 HEROIC VERSE `libaio.dll`, `libaio-iob.dll` and
`libaio-iob2_video.dll` (Ghidra output in `ghidra_work/output`, plus
disassembly of the non-exported functions). Addresses refer to those DLLs.
Everything here is implemented in `src/bi2x/`.

## 1. Transport

* USB CDC device `VID_1CCF&PID_8050` (BIO2 running BI2X firmware). PID 8040 is
  the BIO2 boot loader and 804C is BI2A firmware; libaio only flashes those
  through `aioIob2Bi2x_CreateWriteFirmContext` and otherwise ignores them.
* libaio finds it with `SetupDiGetClassDevs(NULL, ..., PRESENT|ALLCLASSES|DEVICEINTERFACE)`,
  matches `VID_xxxx`/`PID_xxxx` in the instance id and opens the
  `GUID_DEVINTERFACE_USB_DEVICE` interface path with `CreateFile`.
* 115200 8N1, DTR and RTS on, no flow control, reads non-blocking.

## 2. Frame format (`AIO_NODE_IOB2_COMMPKT::Send`, `AIO_NMGR_IOB2_COMM::PacketRecv`)

```
AA  addr  tag  len...  flags  [subst]  data...  [crc7]
```

| field  | meaning |
|--------|---------|
| `AA`   | sync. A raw `AA` anywhere restarts frame decoding, so data never contains it raw |
| addr   | `node << 1 | response`; requests from the host have bit 0 clear, frames from nodes ≠ 0 must have it set |
| tag    | request id, echoed in the response. Host tags skip `00`, `AA`, `FF` |
| len    | 6-bit groups `0xC0 | g` for bits 25, 19, 13, 7 (zero groups are skipped – even inner ones, a quirk that only matters ≥ 0x2000), then `len & 0x7F` |
| flags  | `mode << 5 | encrypted << 4 | crc4` |
| subst  | mode 3 only |
| crc7   | only if len > 0: `crc7(0x7F, data) ^ 0x7F` over the decoded data |

Header checksum (`QueryHeadSum`): CRC-4 with init `0xF` over `addr`, `tag`,
the length bytes (recomputed from the value and the number of length bytes),
`flags & 0xF0`; result `^ 0xF`.

CRCs come from libacc `AC_CRC::Crc4_MakeLGP_C` / `Crc7_MakeLGP_48`:
reflected (LSB-first) CRCs with reversed polynomials `0x0C` (x⁴+x+1) and `0x48`
(x⁷+x³+1). `Crc16_MakeLGP_8408`, from the same family, verifiably matches the
CRC stored in the embedded firmware image, which supports this reading. The
driver still probes the MSB-first alternative if the board does not answer.

Data modes:

| mode | encoding |
|------|----------|
| 0 | `AA`/`FF` sent as `FF, ~b` |
| 1 | frame is thrown away by the host (keep-alive) |
| 2 | raw (used when the payload has no `AA`) |
| 3 | `subst` byte first, then data with `AA` replaced by `subst` (host picks the lowest byte value not in the payload) |
| 4 | LZ compressed, host → node only (libaio uses it when smaller; optional) |

Frames from the node with data `00 7F ...` are notifications and are dropped.

### Encryption

Per-node keystream, enabled for sending once a node has sent an encrypted
frame. Applies to every byte after `flags`. Seed: `tag ^ 0x55` (host → node),
`tag ^ 0xAA` (node → host). Per byte `b`:

```
if ((~b & 0xAA) == 0) return b;                 // AA/FF class untouched
state = state * 0x41C64E6D + 0x3039;
return b ^ (state & (b & 0x80 ? 0x55 : 0x7F));
```

## 3. Bring-up (`AIO_NMGR_IOB2_COMM` state machine, 0x1800168F0)

1. **Reset**: line break for 2750 ms, release, wait 5500 ms (the board may
   re-enumerate on USB; reopen the port).
2. **Sync**: every 20 ms send an empty frame (node 0, new tag) until an empty
   frame with the same tag comes back (≤ 450 tries), then wait for 500 ms of
   silence.
3. **Enumerate**: node 0 ← `00 01`; reply `00 01 s1 s2 …`, one byte per node
   (low nibble 0/1), nodes are numbered from 1.
4. **Firmware info**: node n ← `00 02 81`; reply `00 02 00` + 32 bytes:

   | offset | content |
   |--------|---------|
   | 0..3   | type, BE (`0D060001` = BI2X) |
   | 5..6   | protocol, must be `01 02` |
   | 8..11  | name (`BI2X`) |
   | 0x10..0x1D | configuration; for IIDX `00 00 01 0B 00×8 4A 12` (`AIO_IOB2_BI2X::CheckFirmware`) |
   | 0x1E..0x1F | firmware version; if it differs from the image in `libaio-iob.dll` the game reflashes the board (commands `00 70`, `00 71`) |
5. **Attach** the BI2X node and **upload modules** (every time, they live in RAM):
   `AIO_NCTL_IOB2::LoadModule` for the TDJ module, then the SCI module.

   | request | reply |
   |---------|-------|
   | `00 10 size32BE` | `00 10 00 slot` |
   | `00 13 slot off32BE data[≤64]` | `00 13 00` |
   | `00 78 slot 00 00 00 00` (≤ 1 s) | `00 78 00 id` |

   `id` of the TDJ module prefixes every I/O command below.

Request retry policy in libaio: 3 sends per request; a request carries the
maximum reply length and a processing allowance (2 ms, 1000 ms for `78`)
that feed the timeout.

### Module blobs

`struct MODDATA { const u8 *blob; size_t len; }` – TDJ at `0x18002C270` in
`libaio-iob2_video.dll`, SCI returned by `AIO_IOB2_BI2X::GetModuleDataSci()`
in `libaio-iob.dll`. Blob = `u32 BE unpacked size` + scrambled LZSS:

* descramble: `k = 0; p = c ^ k; k = ~ror8(p, 1)`
* LZSS: 4 KiB zeroed ring, write pointer starts at `0xFEE`; flag byte LSB
  first, 1 = literal, 0 = `b0 b1` → ring position `(b0 & 0xF) << 8 | b1`,
  length `(b0 >> 4) + 3`.

The same packing holds the 128 KiB board firmware (`FIRMDATA` at
`0x18002D580`: 32 byte FIRMINFO + blob), whose CRC-16 at `0x1DFFE` checks out.

## 4. TDJ I/O (`AIO_IOB2_BI2X_TDJ`, libaio-iob2_video.dll)

Each poll is one request with several `id cmd …` sub-commands; the reply
holds one `id cmd status …` per sub-command in the same order.

| cmd | request | reply | when |
|-----|---------|-------|------|
| `12` | 4-byte BE I/O reset flags | `id 12 00` | after `IoReset()` |
| `11` | 17-byte output block | `id 11 st` (st 1 = resend) | outputs changed |
| `22` | – | `id 22 00` | latch tape LEDs after an upload |
| `10` | – | `id 10 00` + 74-byte status | every poll |
| `20` | 256-byte gamma table | `id 20 00` | once, before tape data |
| `21` | `strip start16BE count16BE rgb555LE×count` (several per frame, ≤ 263 bytes) | `id 21 00` each | tape LEDs changed |

libaio only polls when the game calls `aioNodeCtl_UpdateDevicesStatus`; this
driver polls continuously from its own thread.

### Output block (`TDJ+0xE4A`)

| byte | content |
|------|---------|
| 0 | watchdog timer |
| 1 | b0 coin blocker, b1 1P start, b2 2P start, b3 VEFX, b4 EFFECT |
| 2 | coin counter (running count) |
| 3-4 | woofer LED, RGB555 BE |
| 5 | 1P key lamps b0-b6 |
| 6-7 | 1P card reader LED, RGB555 BE |
| 8-9 | 1P turntable LED, RGB555 BE |
| 10 | 1P turntable resistance |
| 11 | 2P key lamps |
| 12-13 | 2P card reader LED |
| 14-15 | 2P turntable LED |
| 16 | 2P turntable resistance |

RGB555 from `0xRRGGBB`: `(c >> 9) & 0x7C00 | (c >> 6) & 0x3E0 | (c >> 3) & 0x1F`.

### Status (74 bytes)

| byte | content |
|------|---------|
| 1 | b0 test, b1 service, b2 coin |
| 2 | b0 1P start, b1 2P start, b2 VEFX, b3 EFFECT |
| 4 / 5 | 1P / 2P turntable (0-255) |
| 6 | (debounced by the host) |
| 8 / 9 | 1P / 2P keys b0-b6 |

`DEVSTATUS` (0xCA bytes, what `aioIob2Bi2xTDJ_GetDeviceStatus` returns):
`[0]` status poll count, `[1]` output acks, `[2]` tape commits, `[3]` raw[0],
`[4..7]` raw[1] bits, `[8..0xF]` raw[2] bits, `[0x10..0x13]` raw[3] bits,
`[0x14]`/`[0x15]` turntables, `[0x16]` raw[6], `[0x17..0x1A]` raw[7] bits
0,1,6,7, `[0x1B..0x21]` 1P keys, `[0x22..0x28]` 2P keys, `[0x29..0x72]` raw
status, `[0x73..0x83]` last output block, `[0x84..0x87]` last reset flags,
`[0x88..0xC9]` raw[8..0x49].

### Tape LEDs

707 LEDs in 17 sections (`start, strip`):
`0,0 19,0 38,1 83,2 128,3 149,4 203,4 214,4 225,4 279,5 296,5 313,6 381,6
449,6 510,7 578,7 646,7 | 707`. Strip bases: `0 38 83 128 149 279 313 510`.
Sections 1, 6, 8, 9, 11, 13, 14, 16 are stored reversed. `SetTapeLedData`
takes R, G, B bytes per LED.

Before sending, `SetTapeLedDataLimited(max = 0xFF, avg = 0x55)` scales the
whole set so the average level stays under `avg` (a power budget):
`level = Σ(r+g+b) * max / (n * 93)`, `scale = level > avg ? avg * (max+1) / level : max+1`,
each 5-bit channel `c * scale >> 8`.

Gamma table: `t[i] = (1 - (1 - i/255)^0.5) * 255`.

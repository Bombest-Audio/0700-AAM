# kick-mcu

Firmware for the MCU on the KICK's control surface, written as portable C++17 and run on the desktop against a simulated board. The MCU scans the pads, buttons, encoders and touch strip and drives the LEDs. The Kick app on the i.MX8M talks to it over `/dev/ttymxc0` (230400 baud, 8N1, no flow control).

This is the transport layer: framing, CRC, the receive parser, the transmit queue and the connection handshake. Input scanning, LEDs and the system and update commands sit on top of it and come in later changes. The hardware is unreleased and the MCU part is not public, so nothing here has run on a real KICK. The wire format below was read from the host code and checked against the emulator, where the unmodified host talks to the Surface.

```
cmake -S Firmware/kick-mcu -B build/mcu -G Ninja
cmake --build build/mcu
ctest --test-dir build/mcu --output-on-failure
```

Add `-DKICK_SANITIZE=ON` for ASan and UBSan. The tests fetch googletest v1.15.2 through FetchContent; pass `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<path>` to build offline.

## Wire format

```
'M' 'T' | type | dir | cmd | size | payload[size-1] | crc8 | trailer (MCU to host only)
```

- `type`: 0 SYSTEM, 1 BUTTON, 2 LED, 3 PAD, 4 KNOBS, 5 MIDI, 6 CV, 7 SLIDER.
- `dir`: 0 request, 1 response. The host only sends requests and ignores responses.
- `size` is the payload length plus one, because it counts the CRC byte.
- `crc8` is polynomial 0x31, initial value 0xFF, no reflection, no final xor, over every byte from `'M'` through the last payload byte.

### The size mismatch in issue #2

`PktMgr` writes `size = payload + 1`, so a frame is `size + 6` bytes. The host receive loop in `input_events.cpp` reads `size + 7`. The extra byte is a trailer: the host hands the first `size + 6` bytes to `PktMgr` and drops the last one unread.

So the two directions differ:

| Direction | Trailer | Payload limit |
|---|---|---|
| MCU to host | one byte after the CRC, `'\n'` (what the Surface sends) | 19 bytes (set by the host's 20-byte buffer) |
| host to MCU | none | 57 bytes (the receive buffer), 33 in practice |

Without the trailer the host swallows the first byte of the next frame. The payload limit comes from the host's `BUFFER_RX_SIZE` of 20: `PushByteToRxFifo` writes `size - 1` bytes into that buffer with no check against its length, so 21 bytes overrun it. By my reading of the code 20 would still fit; I cap at 19 to leave a byte of margin, and nothing the MCU sends is longer than a few bytes. `encodeFrame` refuses anything over 19.

The answer is pinned in three places: the doc comment in `frame_codec.h`, `FrameCodec.McuToHostFrameCarriesATrailerAndHostToMcuDoesNot`, and `PktMgrCrossCheck.HostPktMgrReadsFramesTheFirmwareEncodes`, which feeds the host's real `PktMgr` the firmware's bytes minus the trailer.

### What the host cannot do

These constrain the MCU, and the design follows from them:

- The host cannot resync. It reads whatever is in the serial buffer, and if the buffer holds less than one whole frame it throws everything away. The MCU therefore sends each frame as one contiguous write and only starts a frame when the UART has room for all of it.
- `PktMgr` loses a frame with `size == 1` (no payload): it waits for a data byte that never comes. The MCU parser accepts such frames, and the MCU never sends one.
- `PktMgr` keeps its partial frame in a `static std::vector`. Line noise corrupts its CRC and nothing clears it. That is host code and is not changed here.

## Layout

```
include/kick_mcu/contract.h      wire constants and enums, checked against the host headers
include/kick_mcu/bsp/bsp.h       board interfaces: IUart, IClock, and the ones later layers use
include/kick_mcu/link/           frame_codec, rx_parser, tx_queue, session, link, spsc_ring
src/link/                        implementations
bsp/sim/                         desktop UART and clock for tests
tests/                           googletest suites
```

The core has no vendor headers, no heap use and is built with `-fno-exceptions -fno-rtti`. Only `bsp/<mcu>/` changes when the board arrives.

### `frame_codec`

CRC-8 and `encodeFrame`. The wire direction picks the trailer and the payload limit.

### `rx_parser`

A byte-at-a-time state machine over one 64-byte frame buffer. It never blocks and does bounded work per byte.

- Bytes outside a frame are dropped until `'M' 'T'`. `'M' 'M' 'T'` works.
- A bad CRC, a `size` of 0, or a payload over the 57-byte buffer rejects the frame. Every byte of the rejected frame after its first is then scanned again for a new `'M' 'T'`, so a real frame that started inside the bad one is not lost. This also covers truncation: a short frame absorbs the start of the next one, fails its CRC, and the re-scan finds the next one.
- `expire()` abandons a partial frame the same way. `Link::poll()` calls it after 10 ms with no byte, which flushes a frame stuck waiting for bytes that will not arrive.
- It does not inspect `type`, `dir` or `cmd`.
- Counters: frames, CRC errors, size errors, garbage bytes, re-scans, timeouts.

### `tx_queue`

Fixed rings in four lanes, drained strictly in this order: `System`, `Button`, `PadKnob`, `Aftertouch`. A pad storm can queue faster than 230400 baud drains, and with one FIFO a button press would wait behind it or be dropped when it filled. With lanes the storm loses its own lowest lane first.

- A full lane drops the new frame and counts it, except `Aftertouch`, which drops the oldest because the newest pressure is the one worth sending.
- Order holds inside a lane. A button can overtake an earlier pad event, and aftertouch can trail its pad's release. The input layer avoids the second by not queueing aftertouch for a released pad.
- `pump()` writes a frame only if `IUart::writableBytes()` covers all of it, and stops at a blocked high lane rather than sending a smaller frame from a lower one.
- Worst-case button wait is the frame already in flight plus whatever is in the UART FIFO. The BSP chooses that FIFO depth, so keep it small; at 32 bytes it is 1.4 ms of line. The storm test uses 32.

Depths are 8, 32, 64 and 16 frames, 28 bytes each, about 3.4 KB. They are constants in `tx_queue.h`.

### `session`

The handshake, driven by the host (read from `input_events.cpp`, confirmed against the emulator):

```
host                                        MCU
 |  COMM_INIT [1]                            |
 |------------------------------------------>|   state: Connected
 |            GET_SW_VER [major, minor]      |
 |<------------------------------------------|
 |            CHECK_COMM_STATUS [1]          |
 |<------------------------------------------|
 |  COMM_INIT [2]                            |
 |------------------------------------------>|
 |            CHECK_COMM_STATUS [2]          |
 |<------------------------------------------|
 |  CHECK_COMM_STATUS [2]                    |
 |------------------------------------------>|   state: Streaming
 |            CHECK_COMM_STATUS [2]          |   then every 3 s
 |<------------------------------------------|
```

The `[2]` stage is not in `dcl_contracts.h`. It is what the host sends and expects.

- `COMM_INIT [1]` restarts the sequence from any state and discards event frames left over from the last session.
- `COMM_INIT [0]` and `RECONNECT` drop to `NotConnected`. The host never sends `RECONNECT`; the MCU handles it anyway. The MCU does not start a handshake of its own, since the host is the one that opens the line.
- Until the state is `Streaming`, `Link::send()` refuses event frames and counts them. System frames are always allowed.
- Frames with `dir == response` are ignored.
- There is no link-loss timeout. The host defines none, and nothing says it answers every heartbeat, so the MCU keeps streaming until told otherwise.
- Everything the session does not consume (LED, pad, slider, BPM, shutdown, update commands) goes to a pass-through handler for the layers above.

The version it reports defaults to 1.0, which is what the Surface reports. The host reads two bytes, `[major, minor]`.

### `link`

Owns the receive ring, parser, queue and session, and wires them to an `IUart` and an `IClock`.

- The UART receive interrupt calls one function that pushes a byte into a lock-free single-producer ring. Nothing else runs in the interrupt.
- `poll()` runs on the main loop: drain the ring into the parser, apply the inter-byte timeout, run the heartbeat, pump the queue. It never waits, so the scan loop can call it every pass.
- The ring holds 255 bytes, about 11 ms of line. `poll()` has to run at least that often or bytes are lost and counted in `rxOverruns`.

## Tests

| Suite | What it covers |
|---|---|
| `FrameCodec` | CRC against bytes captured from the emulator, trailer, limits, round trip for every payload length |
| `RxParserTest` | valid, truncated at every point, bad CRC, garbage between frames, back-to-back, bit flips, size 0 and oversize, a real frame inside a rejected one, `expire()`, 200k bytes of noise under ASan |
| `TxQueue` | priority order, whole-frame writes, overflow policy and counters |
| `SessionTest` | each handshake step, heartbeat timing including clock wrap, reconnect, restart, pass-through |
| `LinkTest` | the above through raw UART bytes, inter-byte timeout, ring overflow |
| `Storm` | 16 pads at 1 kHz aftertouch plus strikes and buttons for 3 s on a 230400 baud line: 429 of 429 button frames, 2400 of 2400 pad events, 45029 aftertouch frames dropped and counted |
| `TraceReplay` | `emu/tests/traces/smoke.trace` through the parser and the full link |
| `NoAlloc` | a counting `operator new` sees zero allocations across the receive path and the whole link; `static_assert`s that the types are plain storage |
| `Contract`, `PktMgrCrossCheck` | every enum in `contract.h` equals the host's (`static_assert`), and the host's own `PktMgr.cpp` is compiled in and run against the firmware codec in both directions |

`emu/tests/traces/smoke.trace` was captured by running `KICK_EMU_TRACE=emu/tests/traces/smoke.trace emu/run.sh --script emu/tests/smoke.script` against the unmodified host. The format is described in `emu/README.md`. The replay test checks that the parser yields the frames the Surface decoded, that every frame the Surface sent re-encodes to the same bytes, and that the firmware answers the handshake byte for byte as the Surface does.

## Not verified on hardware

- Baud rate, framing and the handshake are read from the host code and exercised only against the emulator Surface.
- Interrupt and DMA behaviour of the real UART: ring depth, poll interval, TX FIFO size and the button latency that follows from it.
- That the host really tolerates the 10 ms inter-byte timeout and the `'\n'` trailer on silicon the way it does in the emulator. The trailer rule comes from the host's receive loop, not from a captured trace of a real unit.
- The MCU part, its clock and its UART peripheral.

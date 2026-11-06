# Pad protocol golden wire bytes

Generated with `PacketEncoder/packenc.js` (the C encoder produces the same bytes; `pad/test/TestPadProtocol.cpp`
checks the C encoder and decoder against them). The Node tooling (step 9) must check the same arrays.

Layout: `AA AA AA` start signal, `00` separator, varint seq, varint cmd, varint length, payload, CRC-32 big-endian
(over everything after the start signal, stuffing bytes included), `55` terminator.

STATE, seq 5, cmd 1: keys `0x00000005` (key1 and key3 down), one encoder at position `0x00001234`, interval 6500
(`0x1964` us). Payload `05 00 00 00 | 34 12 00 00 | 64 19`.

```
0xAA, 0xAA, 0xAA, 0x00, 0x05, 0x01, 0x0A, 0x05, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, 0x64, 0x19, 0xB3, 0x86, 0x92, 0x41, 0x55
```

HELLO, seq 0, cmd 2: version 1, 6 keys, 1 encoder. Payload `01 06 01`.

```
0xAA, 0xAA, 0xAA, 0x00, 0x00, 0x02, 0x03, 0x01, 0x06, 0x01, 0xD5, 0x86, 0xDE, 0xD7, 0x55
```

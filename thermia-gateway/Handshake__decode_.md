# Exact analysis of the 0x071C / 0x0730 CM930 handshake

**Date:** 2026-10-05  
**Firmware:** Thermia Connect 2.1.105  
**Primary source:** `device_client/cm/modbus_server15.py`  
**Independent in-firmware cross-check:** `/opt/thermia/compliance/tests/th930.py`  
**Crypto library:** `xxtea 2.0.0.post0`

## Status

**FIRMWARE-DERIVED / PROVEN + GENUINE DCM03 CAPTURE VERIFIED**

The exact handshake algorithm recovered from Thermia Connect 2.1.105 reproduces a genuine ATEC + DCM03 FC23 challenge/response pair **byte-for-byte**.

This closes the previously open algorithm/key/byte-order question for this captured protocol family.

---

## 1. Transport primitive

The CM930 is Modbus slave `0x0F`.

Thermia patches `modbus_tk` to support function code `0x17` / FC23 / Read-Write Multiple Registers.

The patched handler conceptually transforms the FC23 PDU into:

1. an FC16 write operation;
2. followed by an FC03 holding-register read operation.

The handshake callback runs on the received request and populates the read area before the FC23 read phase is serviced.

The expected handshake geometry is:

```text
slave        0x0F
function     0x17

read start   0x0730
read count   8

write start  0x071C
write count  8
byte count   16

payload      16-byte challenge
```

The production `_send_handshake_reply()` checks that:

```text
read_count == 8
write_count == 8
```

It parses but does not validate `write_start` in that function.

The older/included compliance test is stricter and checks all of:

```text
read_start  == 1840 / 0x0730
write_start == 1820 / 0x071C
read_count  == 8
write_count == 8
```

That test independently uses the same key, algorithm and byte-order rules.

---

## 2. Exact FC23 request layout

After slave ID and function code, the production callback receives:

```text
offset  size  field
0       2     read_start      big-endian
2       2     read_count      big-endian
4       2     write_start     big-endian
6       2     write_count     big-endian
8       1     byte_count
9       N     challenge bytes
```

The production code extracts:

```python
read_start = int.from_bytes(message[0:2], byteorder="big")
read_size  = int.from_bytes(message[2:4], byteorder="big")
write_size = int.from_bytes(message[6:8], byteorder="big")
byte_count = message[8]
register_values = message[9:9 + byte_count]
```

No conversion of the challenge into 16-bit integers is performed before encryption. The raw 16 challenge bytes are passed to XXTEA.

---

## 3. Exact key material

Firmware key words:

```text
BC6725DC
41839788
ED7ACDF7
CA3393A4
```

The implementation serializes each 32-bit word independently as little-endian:

```text
BC6725DC -> DC 25 67 BC
41839788 -> 88 97 83 41
ED7ACDF7 -> F7 CD 7A ED
CA3393A4 -> A4 93 33 CA
```

Final 16-byte XXTEA key:

```text
dc2567bc88978341f7cd7aeda49333ca
```

This key is passed directly to the installed Python package:

```text
xxtea 2.0.0.post0
```

---

## 4. Exact cipher operation

Production code:

```python
encrypted = xxtea.encrypt(response, byte_key, padding=False)
```

Important consequences:

- input is exactly the 16 raw challenge bytes;
- no application-layer IV exists;
- no nonce is prepended;
- no challenge-derived key modification is visible;
- no salt is used;
- no padding is used;
- the output length stays 16 bytes.

For a 16-byte input, XXTEA operates on four 32-bit words.

The installed library documentation states the default round count is:

```text
6 + 52 // n
```

For `n = 4`:

```text
6 + 52 // 4 = 19 rounds
```

A source-compatible reference implementation using little-endian 32-bit word packing reproduces the genuine captured answer exactly.

---

## 5. Response serialization

The 16 encrypted bytes are split into eight 2-byte chunks:

```python
int.from_bytes(encrypted[i:i+2], byteorder="big")
```

Therefore the encrypted bytes are exposed as eight **big-endian Modbus register values**.

Those eight values are written to the handshake block at the FC23 request's `read_start`.

For the normal request, that means:

```text
0x0730 .. 0x0737
```

The FC23 handler then reads those registers and returns the ordinary FC23 response:

```text
0F 17 10 <16 encrypted bytes> CRC
```

---

## 6. Genuine ATEC + DCM03 validation

Genuine captured request:

```text
0f1707300008071c0008102ee156572b8b84a0bc7a65c72d56185d7840
```

Decoded:

```text
slave        0F
function     17
read_start   0730
read_count   0008
write_start  071C
write_count  0008
byte_count   10 hex = 16 decimal

challenge:
2e e1 56 57 2b 8b 84 a0 bc 7a 65 c7 2d 56 18 5d

CRC:
78 40
```

The Modbus CRC recalculates to `0x4078`, serialized low-byte first as `78 40`.

Firmware algorithm input:

```text
challenge = 2ee156572b8b84a0bc7a65c72d56185d
key       = dc2567bc88978341f7cd7aeda49333ca
padding   = false
rounds    = 19 for this 16-byte block
```

Calculated XXTEA output:

```text
20b8f28a236f28fa0339669e2c6f600d
```

As eight Modbus registers:

```text
0730 = 20B8
0731 = F28A
0732 = 236F
0733 = 28FA
0734 = 0339
0735 = 669E
0736 = 2C6F
0737 = 600D
```

Genuine captured DCM03 response:

```text
0f171020b8f28a236f28fa0339669e2c6f600d57d9
```

The calculated payload is an **exact byte-for-byte match**:

```text
calculated: 20b8f28a236f28fa0339669e2c6f600d
captured:   20b8f28a236f28fa0339669e2c6f600d
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
            16 / 16 bytes identical
```

The response CRC recalculates to `0xD957`, serialized as `57 D9`, also matching the capture.

### Conclusion

The genuine DCM03 response is fully explained by the Thermia Connect CM930 implementation.

No additional session-dependent transformation is required to explain this captured pair.

---

## 7. Handshake state machine

The production CM930 state enum is:

```text
NOT_ACCEPTED = 0
RESPONSE_SENT = 1
ACCEPTED = 2
```

At startup:

```text
NOT_ACCEPTED
```

After a valid-size FC23 challenge is processed:

```text
RESPONSE_SENT
```

The code does **not** set `ACCEPTED` merely because the FC23 response has been returned.

A later FC16 causes acceptance unless its start address is exactly:

```text
0x085F / 2143
or
0x04BA / 1210
```

The production condition is:

```python
start == 1000 or start not in (2143, 1210)
```

Since `1000` is already not one of the excluded values, this simplifies logically to:

```text
accept on any observed FC16 start address except 0x085F or 0x04BA
```

The included compliance-test implementation is narrower: it marks the handshake accepted when it sees an FC16 beginning at `1000 / 0x03E8`.

This difference is a real firmware/source distinction and should not be silently normalized.

---

## 8. Important production-code validation gaps

The production `_send_handshake_reply()` checks only:

```text
read_count == 8
write_count == 8
```

It does **not** explicitly require:

```text
read_start  == 0x0730
write_start == 0x071C
byte_count  == 16
```

before encrypting.

However:

- the FC23 Modbus handler still performs the write operation at the request-specified write address;
- `set_values(HANDSHAKE_BLOCK, read_start, ...)` must resolve inside the configured handshake block;
- the normal controller request uses exactly `071C/8 -> 0730/8`;
- the compliance test explicitly validates the canonical addresses.

Therefore the canonical protocol is clear even though production validation is somewhat permissive.

This permissiveness is an implementation detail, **not** evidence that alternative addresses are part of the intended protocol.

---

## 9. Relationship to existing reverse engineering

Before firmware recovery, the project had already established from genuine captures that:

```text
0x0F FC23
write 071C/count8
read  0730/count8
```

forms the native approval gate.

The missing elements were:

- exact cipher;
- exact key;
- key byte order;
- challenge byte handling;
- response byte/register order;
- whether an additional session-dependent transform existed.

These are now resolved for the recovered CM930/ATEC-DCM03-compatible path.

### Evidence classification

**FIRMWARE-DERIVED / PROVEN**
- slave 0x0F;
- FC23 transport;
- canonical 071C/0730 handshake areas;
- 8-register challenge and response;
- XXTEA;
- embedded key;
- key serialization;
- no padding;
- response register serialization;
- three-state handshake state machine.

**GENUINE CAPTURE VERIFIED**
- the exact recovered algorithm produces the exact genuine ATEC/DCM03 response for a captured challenge;
- captured request and response CRCs also validate.

**STRONG CONCLUSION**
- no unknown challenge-response algorithm remains for this captured protocol path;
- no additional per-session secret is needed to reproduce the validated pair.

**OPEN / NOT YET LOCALLY PROVEN**
- whether the local iTec XTR M accepts an emulated response generated by this same algorithm in a live approval exchange;
- whether all firmware/profile generations use the same fixed key;
- whether Connect 2.0.18 uses the same implementation and key;
- whether any controller model adds policy/state requirements around an otherwise correct response.

---

## 10. Safe next step

No live transmission is needed to improve confidence further.

Highest-value offline checks:

1. test every other genuine FC23 challenge/response pair in the capture corpus against this algorithm;
2. compare the 2.0.18 firmware implementation and key;
3. add the handshake algorithm to the protocol reference model and golden-trace regression;
4. only after those checks consider a bounded local XTR approval experiment.


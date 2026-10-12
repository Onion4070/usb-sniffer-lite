"""
Usage: python pc_receiver.py COM5

Requires: pip install pyserial. Ctrl+C sends 'p'.
Text diagnostics on the same CDC port are ignored while searching for frames.
"""
import collections
import sys
import time
import serial

MAX_PAYLOAD = 192
MAGIC = b"\xaa\x55"


def crc16_ccitt_false(data):
    crc = 0xffff
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def main(port):
    counts = collections.Counter()
    partial = bytearray()
    start = time.monotonic()
    last = start
    corrupted = 0
    with serial.Serial(port, 115200, timeout=0.1) as ser:
        ser.write(b's')
        try:
            while True:
                partial.extend(ser.read(4096))
                while True:
                    marker = partial.find(MAGIC)
                    if marker < 0:
                        partial[:] = b'\xaa' if partial.endswith(b'\xaa') else b''
                        break
                    if marker:
                        del partial[:marker]
                    if len(partial) < 11:
                        break
                    length = int.from_bytes(partial[9:11], 'little')
                    if length > MAX_PAYLOAD:
                        del partial[0]
                        corrupted += 1
                        continue
                    frame_length = length + 13
                    if len(partial) < frame_length:
                        break
                    actual = int.from_bytes(partial[11+length:13+length], 'little')
                    expected = crc16_ccitt_false(partial[2:11+length])
                    if actual != expected:
                        corrupted += 1
                        del partial[0]
                        continue
                    addr, ep, pid = partial[2:5]
                    timestamp_us = int.from_bytes(partial[5:9], 'little')
                    payload = bytes(partial[11:11+length])
                    # Report metadata and payload are ready for downstream processing.
                    counts[(addr, ep)] += 1
                    del partial[:frame_length]
                now = time.monotonic()
                if now - last >= 3:
                    elapsed = now - start
                    summary = ' '.join(
                        f'{a}/{ep}: {n/elapsed:.1f}Hz ({n})'
                        for (a,ep),n in sorted(counts.items())
                    )
                    print(f'{elapsed:.1f}s {summary} frame_errors={corrupted}', flush=True)
                    last = now
        except KeyboardInterrupt:
            ser.write(b'p')
            print('\nStop command sent.')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: python pc_receiver.py COM5')
    main(sys.argv[1])

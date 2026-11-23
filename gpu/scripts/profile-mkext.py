#!/usr/bin/env python3
"""Publish and restore the tested WindowServer mkexts using a local Tiger component."""

import argparse
import hashlib
from pathlib import Path
import struct
import zlib

PROFILES = {
    "XWAU.MKE": "c173f317c13180d64ddd719f355128c32ecfe2fc47d814e32ab14e062bca00cd",
    "XWOV.MKE": "a10b60034ddd9fec5164834cf4bd2c1914847b224adaa97f7c42e6ddacaa6bac",
}

# Exact encoded plist and executable ranges of IOGraphicsFamily 1.4.2.
TIGER_PAYLOADS = (
    (233600, 939, "6fdaec87518b166ced2284e4d03a5746d68902d177ac6cbb33b61aa58d2ef7aa"),
    (234560, 139577, "c0d080da97d3278ac3ae1b99d6adee55ae35d2cd414e7b34a4587e67493c31d8"),
)
MKEXT_HEADER = struct.Struct(">8s4I")
MKEXT_FILE = struct.Struct(">4I")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def local_tiger_payloads(reference):
    if len(reference) < 32:
        raise ValueError("Reference mkext is truncated")
    magic, length, checksum, version, count = MKEXT_HEADER.unpack_from(reference)
    end_of_table = 32 + 32 * count
    if (magic != b"MKXTMOSX" or version != 0x01008000 or length != len(reference)
            or end_of_table > length or zlib.adler32(reference[16:]) != checksum):
        raise ValueError("Reference is not a valid mkext v1 archive")

    for record in range(32, end_of_table, 32):
        payloads = []
        for field, (_, expected_size, expected_digest) in enumerate(TIGER_PAYLOADS):
            start, compressed, _, _ = MKEXT_FILE.unpack_from(reference, record + 16 * field)
            if compressed != expected_size or start < end_of_table or start + compressed > length:
                break
            data = reference[start:start + compressed]
            if digest(data) != expected_digest:
                break
            payloads.append(data)
        if len(payloads) == len(TIGER_PAYLOADS):
            return payloads
    raise ValueError("Reference lacks the exact tested Tiger IOGraphicsFamily 1.4.2 payloads")


def split(profile, data):
    if digest(data) != PROFILES[profile]:
        raise ValueError("Input does not match the tested mkext checksum")
    for start, length, expected_digest in TIGER_PAYLOADS:
        if digest(data[start:start + length]) != expected_digest:
            raise ValueError("Tiger payload does not match its expected placement")
        data[start:start + length] = bytes(length)
    data[:16] = bytes(16)
    return data


def restore(profile, data, reference):
    if len(data) != 398272 or any(data[:16]):
        raise ValueError("Input is not a WindowServer mkext template")
    for (start, length, _), payload in zip(TIGER_PAYLOADS, local_tiger_payloads(reference)):
        if any(data[start:start + length]):
            raise ValueError("Template contains unexpected Tiger payload data")
        data[start:start + length] = payload
    struct.pack_into(">8s2I", data, 0, b"MKXTMOSX", len(data), zlib.adler32(data[16:]))
    if digest(data) != PROFILES[profile]:
        raise ValueError("Restored archive does not match the tested mkext checksum")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("split", "restore"))
    parser.add_argument("profile", choices=PROFILES)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--reference", type=Path, help="local mkext with the matching Tiger payloads")
    args = parser.parse_args()
    if (args.action == "restore") != bool(args.reference):
        parser.error("--reference is required for restore and applies only to restore")
    try:
        data = bytearray(args.input.read_bytes())
        output = (split(args.profile, data) if args.action == "split"
                  else restore(args.profile, data, args.reference.read_bytes()))
        with args.output.open("xb") as file:
            file.write(output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"{digest(output)}  {args.output}")


if __name__ == "__main__":
    main()

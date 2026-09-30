#!/usr/bin/env python3
"""Generate test-only UCS-2 CBS PDUs or replay exact binary modem payloads."""
import argparse
import json
import secrets
import socket
import struct
import time
from pathlib import Path


def make_pdus(text, channel, serial, wire_format):
    if not text.startswith("TEST ONLY"):
        raise ValueError("generated messages must start with TEST ONLY")
    if not 0 <= channel <= 65535 or not 0 <= serial <= 65535:
        raise ValueError("channel and serial must fit in 16 bits")
    if any(ord(c) > 0xffff or 0xd800 <= ord(c) <= 0xdfff for c in text):
        raise ValueError("CBS UCS-2 text requires non-surrogate BMP characters")
    encoded = text.encode("utf-16-be")
    pages = [encoded[i:i + 82] for i in range(0, len(encoded), 82)]
    if not 1 <= len(pages) <= 15:
        raise ValueError("message must contain 1 to 15 pages")
    padded = [p + b"\x00\x0d" * ((82 - len(p)) // 2) for p in pages]
    if wire_format == "gsm":
        return [struct.pack(">HHBB", serial, channel, 0x48,
                            ((i + 1) << 4) | len(pages)) + p
                for i, p in enumerate(padded)]
    # 3GPP UMTS CBS: message type, identifier, serial, DCS, page count,
    # then fixed-size page data and each page's actual octet length.
    return [struct.pack(">BHHBB", 1, channel, serial, 0x48, len(pages)) +
            b"".join(p + bytes([len(raw)]) for p, raw in zip(padded, pages))]


def replay(endpoint, files, interval):
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        sock.bind("\0cbs-replay-" + secrets.token_hex(12))
        sock.settimeout(5)
        sock.connect(str(endpoint))
        for i, file in enumerate(files):
            pdu = file.read_bytes()
            if not 1 <= len(pdu) <= 65535:
                raise ValueError("invalid PDU length: " + str(file))
            if i:
                time.sleep(interval)
            sock.send(pdu)
            receipt = sock.recv(256).decode("ascii")
            print(json.dumps({"file": str(file), "bytes": len(pdu),
                              "receipt": receipt}), flush=True)
            if receipt != "DISPATCHED":
                raise RuntimeError(receipt)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    generate = sub.add_parser("generate", help="write fixtures; does not inject")
    generate.add_argument("directory", type=Path)
    generate.add_argument("--text", default="TEST ONLY - local CBS injection")
    generate.add_argument("--channel", type=lambda s: int(s, 0), default=4370)
    generate.add_argument("--serial", type=lambda s: int(s, 0))
    generate.add_argument("--format", choices=("gsm", "umts"), default="gsm")
    send = sub.add_parser("replay", help="inject exact binary PDUs")
    send.add_argument("files", type=Path, nargs="+")
    send.add_argument("--socket", type=Path,
                      default=Path("/run/ofono-cbs-inject/ril_0.sock"))
    send.add_argument("--interval", type=float, default=0.2)
    args = parser.parse_args()
    try:
        if args.command == "replay":
            if args.interval < 0:
                raise ValueError("interval must be nonnegative")
            replay(args.socket, args.files, args.interval)
        else:
            serial = args.serial if args.serial is not None else (
                0x4000 | (secrets.randbelow(1024) << 4))
            pdus = make_pdus(args.text, args.channel, serial, args.format)
            args.directory.mkdir(parents=True, exist_ok=False)
            for i, pdu in enumerate(pdus, 1):
                (args.directory / ("page-%02d.pdu" % i)).write_bytes(pdu)
            manifest = {"text": args.text, "channel": args.channel,
                        "serial": serial, "format": args.format,
                        "pdus": [p.hex() for p in pdus]}
            (args.directory / "manifest.json").write_text(
                json.dumps(manifest, indent=2) + "\n")
            print(json.dumps(manifest, indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()

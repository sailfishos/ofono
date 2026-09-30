#!/usr/bin/env python3
"""Bounded on-device CBS replay check; leaves its alert for UI inspection.

Requires a separately created history backup and explicitly enabled test socket.
Never inserts or edits database rows. Cleanup uses the production D-Bus API.
"""
import argparse
import importlib.util
import json
import os
import re
from pathlib import Path
import sqlite3
import subprocess
import time


def user_call(member, *arguments):
    return subprocess.check_output([
        "runuser", "-u", "defaultuser", "--", "env",
        "XDG_RUNTIME_DIR=/run/user/100000",
        "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket",
        "dbus-send", "--session", "--print-reply", "--dest=org.nemomobile.voicecall",
        "/cellbroadcast", "org.nemomobile.voicecall.CellBroadcast." + member,
        *arguments], text=True, timeout=5)


def connect(path):
    connection = sqlite3.connect("file:" + str(path) + "?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    return connection


def display_state():
    reply = subprocess.check_output([
        "dbus-send", "--system", "--print-reply", "--dest=com.nokia.mce",
        "/com/nokia/mce/request", "com.nokia.mce.request.get_display_status"],
        text=True, timeout=3)
    match = re.search(r'string "(on|dim|off)"', reply)
    assert match, reply
    return match.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--database", type=Path, default=Path(
        "/home/defaultuser/.local/share/nemomobile/voicecall/cellbroadcast/alerts.sqlite"))
    parser.add_argument("--serial", type=lambda s: int(s, 0), default=0x6d10)
    parser.add_argument("--format", choices=("gsm", "umts"), default="gsm")
    parser.add_argument("--text", default="TEST ONLY - raw modem input 20260930")
    parser.add_argument("--cleanup", action="store_true")
    parser.add_argument("--require-screen-off", action="store_true")
    args = parser.parse_args()
    assert os.geteuid() == 0, "run as root"
    backup = args.directory.parent / "before.sqlite"
    assert backup.exists(), "preserve baseline history before testing"
    db = connect(args.database)
    baseline = connect(backup)
    original_ids = {row[0] for row in baseline.execute("select id from cellbroadcast_alerts")}
    if args.cleanup:
        result = json.loads((args.directory / "result.json").read_text())
        record_id = result["record_id"]
        assert record_id not in original_ids
        rows = list(db.execute("select body from cellbroadcast_messages where alert_id=?",
                               (record_id,)))
        assert rows and all(row[0].startswith("TEST ONLY") for row in rows)
        user_call("acknowledgeAlert", "uint64:" + str(record_id))
        assert "boolean true" in user_call("deleteAlert", "uint64:" + str(record_id))
        print(json.dumps({"deleted_test_record": record_id}))
        return

    spec = importlib.util.spec_from_file_location(
        "replay", Path(__file__).with_name("cbs-replay.py"))
    replay = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(replay)
    assert not list(db.execute("select id from cellbroadcast_messages where serial_number=?",
                               (args.serial,))), "use a fresh serial number"
    args.directory.mkdir(mode=0o700)
    pdus = replay.make_pdus(args.text, 4370, args.serial, args.format)
    files = []
    for i, pdu in enumerate(pdus):
        path = args.directory / ("page-%02d.pdu" % (i + 1))
        path.write_bytes(pdu)
        files.append(path)
    (args.directory / "manifest.json").write_text(json.dumps({
        "text": args.text, "serial": args.serial, "channel": 4370,
        "format": args.format, "pdus": [p.hex() for p in pdus]}, indent=2))

    log = (args.directory / "ofono-signals.log").open("w")
    monitor = subprocess.Popen([
        "dbus-monitor", "--system", "type='signal',interface='org.ofono.CellBroadcast'"],
        stdout=log, stderr=subprocess.STDOUT)
    record_id = None
    try:
        time.sleep(0.3)
        assert monitor.poll() is None, "signal monitor failed"
        if args.require_screen_off:
            before = display_state()
            (args.directory / "display-before.txt").write_text(before + "\n")
            assert before == "off", "display must already be off before injection"
        replay.replay(Path("/run/ofono-cbs-inject/ril_0.sock"), files[::-1], 0.2)
        deadline = time.monotonic() + 6
        rows = []
        while time.monotonic() < deadline:
            rows = list(db.execute("select * from cellbroadcast_messages where serial_number=?",
                                   (args.serial,)))
            if rows:
                break
            time.sleep(0.1)
        assert len(rows) == 1, "expected one decoded, stored message"
        message = dict(rows[0])
        assert message["body"] == args.text, "decoded text mismatch"
        assert message["alert_id"] not in original_ids
        record_id = message["alert_id"]
        # Record ownership immediately, so cleanup remains possible on failure.
        result = {"record_id": message["alert_id"], "serial": args.serial,
                  "decoded_text": message["body"], "page_count": message["page_count"],
                  "properties": json.loads(message["properties_json"])}
        (args.directory / "result.json").write_text(json.dumps(result, indent=2))
        active = user_call("activeAlert")
        (args.directory / "active-alert.txt").write_text(active)
        assert re.search(r'string "RecordId"\s+variant\s+(?:u?int64)\s+' +
                         str(message["alert_id"]) + r'\b', active)
        assert args.text in active
        assert "boolean true" in user_call("silenceAlert", "uint64:" + str(record_id))
        if args.require_screen_off:
            deadline = time.monotonic() + 3
            after = display_state()
            while after != "on" and time.monotonic() < deadline:
                time.sleep(0.1)
                after = display_state()
            (args.directory / "display-after.txt").write_text(after + "\n")
            assert after == "on", "alert did not wake the display"
            result.update(display_before=before, display_after=after)
        snapshot = [tuple(row) for row in db.execute(
            "select * from cellbroadcast_messages where alert_id=?", (message["alert_id"],))]
        replay.replay(Path("/run/ofono-cbs-inject/ril_0.sock"), files, 0.2)
        time.sleep(0.5)
        assert snapshot == [tuple(row) for row in db.execute(
            "select * from cellbroadcast_messages where alert_id=?", (message["alert_id"],))], \
            "duplicate changed stored message"
        result["exact_duplicate_suppressed"] = True
    finally:
        try:
            if record_id is not None:
                user_call("silenceAlert", "uint64:" + str(record_id))
        finally:
            monitor.terminate()
            monitor.wait(timeout=3)
            log.close()
    signals = (args.directory / "ofono-signals.log").read_text()
    count = signals.count("member=IncomingBroadcastWithProperties")
    assert count == 1, "expected exactly one detailed oFono signal, got %d" % count
    result["detailed_ofono_signals"] = count
    (args.directory / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

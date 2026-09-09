#!/usr/bin/env python3
"""Auto-reconnecting serial monitor for the T-Watch Ultra (13:37 firmware).

The watch exposes a *native USB CDC* port (VID 303A). On every reset the port
disappears and re-enumerates, so PlatformIO's / VSCode's monitor loses it and
you never catch the boot banner. This monitor reopens the port as soon as it
comes back, so reboots (and the boot log) are never missed.

Usage:
    python tools/watch_monitor.py                 # auto-detect the 303A CDC port
    python tools/watch_monitor.py COM13           # force a port
    python tools/watch_monitor.py --cmd status    # send one console cmd, print reply, exit
    python tools/watch_monitor.py --reboot        # send 'reboot' then follow the boot log

Interactive: anything you type + Enter is sent to the firmware console
(help, status, sys, uptime, gps, wifi, reboot, ...). Ctrl-C to quit.
"""
import sys, time, threading
import serial
from serial.tools import list_ports

BAUD = 115200


def find_port():
    for p in list_ports.comports():
        # Espressif native-USB CDC: VID 0x303A
        if (p.vid == 0x303A) or ("303A" in (p.hwid or "").upper()):
            return p.device
    return None


def open_port(port):
    """Block until the CDC port exists, then open it (DTR high, RTS low)."""
    while True:
        try:
            s = serial.Serial(port, BAUD, timeout=0.2)
            s.dtr = True
            s.rts = False
            return s
        except Exception:
            time.sleep(0.2)


def main():
    args = sys.argv[1:]
    one_cmd = None
    do_reboot = False
    port = None
    for a in list(args):
        if a == "--reboot":
            do_reboot = True
        elif a == "--cmd":
            i = args.index(a)
            one_cmd = args[i + 1]
        elif not a.startswith("--") and args[max(0, args.index(a) - 1)] != "--cmd":
            port = a

    if not port:
        port = find_port()
        if not port:
            print("Nessuna porta CDC 303A trovata. Collega l'orologio o passa la COM a mano.")
            sys.exit(1)
    print(f"[monitor] porta {port} @ {BAUD}  (Ctrl-C per uscire)")

    s = open_port(port)

    # one-shot command mode
    if one_cmd is not None:
        time.sleep(0.2); s.reset_input_buffer()
        s.write((one_cmd + "\n").encode()); s.flush()
        time.sleep(0.8)
        sys.stdout.write(s.read(8192).decode("utf-8", "replace"))
        return

    if do_reboot:
        s.write(b"reboot\n"); s.flush()
        time.sleep(0.3)

    # background thread: read stdin, forward lines to the console
    def stdin_pump():
        try:
            for line in sys.stdin:
                try:
                    s.write(line.encode()); s.flush()
                except Exception:
                    pass
        except Exception:
            pass
    threading.Thread(target=stdin_pump, daemon=True).start()

    # main read loop with auto-reconnect across resets
    while True:
        try:
            data = s.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
        except (serial.SerialException, OSError):
            print("\n[monitor] porta caduta (reset?) — riconnetto...", flush=True)
            try: s.close()
            except Exception: pass
            s = open_port(port)
            print(f"[monitor] riconnesso a {port}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[monitor] chiuso.")

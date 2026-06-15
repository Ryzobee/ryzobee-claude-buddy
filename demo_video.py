"""
Claude Hardware Buddy — video demo driver
=========================================

Sends a choreographed JSON sequence over USB serial to the device, simulating
a real Claude desktop session so you can film all the buddy states in one take.

Sequence (~25s, plus approval wait time):
    T+0s   IDLE       one session, "Claude online"
    T+4s   BUSY       3 running tasks → busy animation
    T+10s  ATTENTION  waiting=1 → red LED + attention pose
    T+12s  PROMPT     approval request → device beeps, shows tool name
           ⤷ YOU PRESS THE BUTTON HERE
             short = approve (HEART if <5s), hold = deny
    T+approve+3s   CELEBRATE  completed=true + token jump → level up + celebrate
    T+approve+8s   IDLE       back to normal, scene ends

Usage:
    pip install pyserial
    python demo_video.py                       # defaults to COM14
    python demo_video.py --port COM7
    python demo_video.py --prompt-tool "Git: force push main"
    python demo_video.py --countdown 5         # more time to hit record

Notes:
- The device must be plugged in via USB. Close the Arduino Serial Monitor
  first — Windows only allows one process to hold a COM port.
- If the desktop Claude bridge is connected over BLE, that's fine; this
  script only writes to USB. The device merges both streams.
"""

import argparse
import json
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed. Run:  pip install pyserial")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Serial I/O helpers
# ---------------------------------------------------------------------------

def send_json(ser, obj):
    line = json.dumps(obj, ensure_ascii=False) + "\n"
    ser.write(line.encode("utf-8"))
    ser.flush()
    print(f"   → {json.dumps(obj, ensure_ascii=False)}")


# Background reader: prints whatever the device sends back, and signals
# when an approval response arrives.
permission_event = threading.Event()
permission_decision = [None]
stop_reader = threading.Event()


def reader_loop(ser):
    buf = b""
    while not stop_reader.is_set():
        try:
            data = ser.read(256)
        except Exception:
            break
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            text = raw.decode("utf-8", errors="replace").strip()
            if not text:
                continue
            if text.startswith("{"):
                try:
                    msg = json.loads(text)
                    if msg.get("cmd") == "permission":
                        permission_decision[0] = msg.get("decision")
                        permission_event.set()
                        print(f"   ← APPROVAL RECEIVED: {msg.get('decision')} "
                              f"(id={msg.get('id')})")
                        continue
                except json.JSONDecodeError:
                    pass
            # Non-JSON log line from the device — print dimmed
            print(f"   ← {text}")


# ---------------------------------------------------------------------------
# Main demo sequence
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM14", help="serial port (default COM14)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--countdown", type=int, default=3,
                    help="seconds to wait before starting (gives you time to hit record)")
    ap.add_argument("--prompt-tool", default="Bash: rm -rf node_modules",
                    help="tool name shown in the approval prompt")
    ap.add_argument("--prompt-hint", default="clean install")
    ap.add_argument("--approval-timeout", type=int, default=30,
                    help="how long to wait for button press before forcing the next scene")
    args = ap.parse_args()

    print(f"\nOpening {args.port} @ {args.baud}...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as e:
        print(f"\nERROR: could not open {args.port}: {e}")
        print("Is the Arduino Serial Monitor or another program holding the port?")
        sys.exit(1)

    time.sleep(0.5)  # let USB-CDC settle
    ser.reset_input_buffer()

    reader_thread = threading.Thread(target=reader_loop, args=(ser,), daemon=True)
    reader_thread.start()

    # ---- Countdown ----
    print("\n=== Claude Buddy Video Demo ===")
    print(f"Filming starts in {args.countdown}s. Hit record now.\n")
    for i in range(args.countdown, 0, -1):
        print(f"   {i}...")
        time.sleep(1)
    print("\n--- ACTION ---\n")

    try:
        # ---- T+0s: IDLE / connected baseline ----
        # First tokens packet just latches the baseline (no level up yet).
        print("[T+0s ] IDLE — connecting to Claude")
        send_json(ser, {
            "total": 1, "running": 0, "waiting": 0,
            "completed": False,
            "tokens": 1000,
            "tokens_today": 1000,
            "msg": "Claude online",
        })
        send_json(ser, {
            "entries": [
                "Connected to Claude desktop",
                "Watching your sessions...",
            ],
        })
        time.sleep(4)

        # ---- T+4s: BUSY ----
        print("\n[T+4s ] BUSY — 3 tasks running (running>=3 triggers busy pose)")
        send_json(ser, {
            "total": 4, "running": 3, "waiting": 0,
            "completed": False,
            "msg": "3 tasks running",
        })
        send_json(ser, {
            "entries": [
                "Refactoring auth module",
                "Running test suite",
                "Writing documentation",
            ],
        })
        time.sleep(6)

        # ---- T+10s: ATTENTION (red LED pulse begins) ----
        print("\n[T+10s] ATTENTION — needs approval (red LED pulse)")
        send_json(ser, {
            "total": 4, "running": 2, "waiting": 1,
            "completed": False,
            "msg": "Needs approval",
        })
        time.sleep(2)

        # ---- T+12s: PROMPT — approval request, the dramatic beat ----
        print(f"\n[T+12s] PROMPT — '{args.prompt_tool}'")
        print("        >>> PRESS THE DEVICE BUTTON NOW <<<")
        print("            short press = Approve     long hold = Deny")
        send_json(ser, {
            "total": 4, "running": 2, "waiting": 1,
            "completed": False,
            "msg": "Needs approval",
            "prompt": {
                "id": "demo-001",
                "tool": args.prompt_tool,
                "hint": args.prompt_hint,
            },
        })

        # Wait for the user's button press, or time out gracefully.
        print(f"\n   ...waiting up to {args.approval_timeout}s for button press...\n")
        got = permission_event.wait(timeout=args.approval_timeout)

        if got:
            decision = permission_decision[0]
            print(f"\n[✓]    Got '{decision}' from device")
        else:
            decision = "once"
            print(f"\n[!]    No button press — continuing as if approved")

        # Brief pause so any HEART one-shot (fast approval) plays before we
        # switch state.
        time.sleep(3)

        # ---- After approval: clear prompt + CELEBRATE + level up ----
        if decision == "deny":
            print("\n[+3s] DENIED — quietly back to idle, no celebrate")
            send_json(ser, {
                "total": 3, "running": 2, "waiting": 0,
                "completed": False,
                "msg": "Denied — moving on",
            })
        else:
            # tokens delta = 55000 - 1000 = 54000 → crosses a 50K boundary,
            # which queues a CELEBRATE one-shot via statsPollLevelUp().
            # completed:true also forces the celebrate state.
            print("\n[+3s] CELEBRATE — task complete + level up")
            send_json(ser, {
                "total": 3, "running": 2, "waiting": 0,
                "completed": True,
                "tokens": 55000,
                "tokens_today": 55000,
                "msg": "Task complete!",
            })
            send_json(ser, {
                "entries": [
                    "Task complete",
                    "Tests passed",
                    "+50K tokens — level up!",
                ],
            })
        time.sleep(5)

        # ---- Back to IDLE: clean ending shot ----
        print("\n[+8s] IDLE — scene ends")
        send_json(ser, {
            "total": 2, "running": 0, "waiting": 0,
            "completed": False,
            "msg": "All done",
        })
        send_json(ser, {
            "entries": [
                "All done.",
                "Watching for the next session...",
            ],
        })
        time.sleep(3)

        print("\n--- CUT ---\n")
        print("Demo complete. You can re-run this script for another take.")

    except KeyboardInterrupt:
        print("\n\nInterrupted — cleaning up.")
    finally:
        stop_reader.set()
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()

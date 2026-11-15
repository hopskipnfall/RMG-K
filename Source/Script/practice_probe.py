#!/usr/bin/env python3
"""Interactive probe for the RMG-K practice protocol (docs/PRACTICE_PROTOCOL.md).

A throwaway developer tool for exercising the emulator side by hand: it
listens like a practice server, acknowledges the emulator, prints a one-line
summary of the live match every few frames, and lets you type commands that
are sent in the reply to the next frame. It is NOT the practice server and
implements no drill logic.

    python3 practice_probe.py [--port 46464] [--every 30]

Commands (typed on stdin, queued for the next reply):
    pos PORT X Y          teleport (airborne fighters only)
    vel PORT VX VY
    damage PORT PERCENT
    stocks PORT N
    shield PORT N
    save SLOT             SaveState
    load SLOT             LoadState (state writes in the same reply are rejected)
    puppet PORT BUTTONS_HEX X Y FRAMES    e.g. puppet 1 0x8000 0 0 5
    setting KEY VALUE     SetSetting (key 0 = hitstun .. 38)
    rng SEED
    help | quit
"""
import argparse
import queue
import socket
import struct
import sys
import threading

PROTOCOL_VERSION = 1
HELLO, MATCH_BEGIN, FRAME, MATCH_END = 0x01, 0x02, 0x03, 0x04
HELLO_ACK, COMMANDS = 0x81, 0x82
RESULT_NAMES = {0: "Ok", 1: "Unknown", 2: "Malformed", 3: "Rejected", 4: "SlotEmpty", 5: "Failed", 6: "Pending"}
EV_INPUT, EV_STATE = 0x03, 0x04


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("emulator closed the connection")
        data += chunk
    return data


def recv_message(sock):
    (length,) = struct.unpack("<I", recv_exact(sock, 4))
    body = recv_exact(sock, length)
    return body[0], body[1:]


def send_message(sock, msg_type, payload=b""):
    sock.sendall(struct.pack("<IB", 1 + len(payload), msg_type) + payload)


def parse_hello(payload):
    version, = struct.unpack_from("<H", payload, 0)
    good_name = payload[2:66].split(b"\0", 1)[0].decode()
    schema, = struct.unpack_from("<I", payload, 66)
    count = payload[70]
    sizes = {}
    for i in range(count):
        code, size = struct.unpack_from("<BH", payload, 71 + 3 * i)
        sizes[code] = size
    return version, good_name, schema, sizes


def walk_events(stream, sizes):
    """Yield (code, payload_bytes) for each event in an event stream."""
    offset = 0
    while offset < len(stream):
        code = stream[offset]
        size = sizes.get(code)
        if size is None:
            raise ValueError("event code 0x%02x has no declared size" % code)
        yield code, stream[offset + 1:offset + 1 + size]
        offset += 1 + size


def parse_frame(payload, sizes):
    frame, flags, result_count = struct.unpack_from("<iBB", payload, 0)
    results = list(payload[6:6 + result_count])
    events = list(walk_events(payload[6 + result_count:], sizes))
    return frame, flags, results, events


def describe_state(payload):
    """One StateFrame event (RMGR_SPEC.md 5.2)."""
    frame, port, char_id, action = struct.unpack_from("<iBBH", payload, 0)
    px, py = struct.unpack_from("<ff", payload, 0x08)
    vx, vy = struct.unpack_from("<ff", payload, 0x14)
    damage, = struct.unpack_from("<I", payload, 0x1C)
    stocks, = struct.unpack_from("<b", payload, 0x20)
    grounded = payload[0x22]
    return "P%d char=0x%02x act=0x%03x pos=(%.1f,%.1f) vel=(%.2f,%.2f) dmg=%d stocks=%d %s" % (
        port, char_id, action, px, py, vx, vy, damage, stocks, "air" if grounded else "ground")


def command(code, payload):
    return struct.pack("<BH", code, len(payload)) + payload


def build_player_state(port, mask, **fields):
    return command(0x01, struct.pack(
        "<BIffffiIibiBBB", port, mask,
        fields.get("px", 0.0), fields.get("py", 0.0), fields.get("vx", 0.0), fields.get("vy", 0.0),
        fields.get("facing", 0), fields.get("damage", 0), fields.get("shield", 0),
        fields.get("stocks", 0), fields.get("charspecific", 0),
        fields.get("jumps", 0), fields.get("hurtbox", 0), fields.get("special", 0)))


def parse_command_line(line):
    """Turns one stdin line into an encoded command, or raises ValueError."""
    parts = line.split()
    if not parts:
        raise ValueError("empty")
    name, args = parts[0], parts[1:]
    if name == "pos":
        return build_player_state(int(args[0]), 0b11, px=float(args[1]), py=float(args[2]))
    if name == "vel":
        return build_player_state(int(args[0]), 0b1100, vx=float(args[1]), vy=float(args[2]))
    if name == "damage":
        return build_player_state(int(args[0]), 1 << 5, damage=int(args[1]))
    if name == "stocks":
        return build_player_state(int(args[0]), 1 << 7, stocks=int(args[1]))
    if name == "shield":
        return build_player_state(int(args[0]), 1 << 6, shield=int(args[1]))
    if name == "save":
        return command(0x05, struct.pack("<B", int(args[0])))
    if name == "load":
        return command(0x06, struct.pack("<B", int(args[0])))
    if name == "puppet":
        return command(0x02, struct.pack("<BHbbH", int(args[0]), int(args[1], 0), int(args[2]), int(args[3]), int(args[4])))
    if name == "setting":
        return command(0x03, struct.pack("<HB", int(args[0]), int(args[1])))
    if name == "rng":
        return command(0x04, struct.pack("<i", int(args[0])))
    raise ValueError("unknown command %r (try 'help')" % name)


def build_commands(frame, encoded):
    return struct.pack("<iB", frame, len(encoded)) + b"".join(encoded)


def serve(conn, pending, every, out=print):
    version, good_name, schema, sizes = parse_hello(recv_message(conn)[1])
    out("hello: protocol=%d rom=%s schema=%d" % (version, good_name, schema))
    send_message(conn, HELLO_ACK, struct.pack("<HB", PROTOCOL_VERSION, 1 if version == PROTOCOL_VERSION else 0))

    last_results = []
    while True:
        msg_type, payload = recv_message(conn)
        if msg_type == MATCH_BEGIN:
            out("match begin (serial %d)" % struct.unpack_from("<I", payload, 0))
        elif msg_type == MATCH_END:
            out("match end (serial %d)" % struct.unpack_from("<I", payload, 0))
        elif msg_type == FRAME:
            frame, flags, results, events = parse_frame(payload, sizes)
            if results:
                out("frame %d results: %s" % (frame, ", ".join(RESULT_NAMES.get(r, str(r)) for r in results)))
            if flags & 1:
                out("frame %d: state loaded" % frame)
            if flags & 2:
                out("frame %d: previous reply timed out" % frame)
            if frame % every == 0:
                for code, event in events:
                    if code == EV_STATE:
                        out("frame %d %s" % (frame, describe_state(event)))
            encoded = []
            while True:
                try:
                    encoded.append(pending.get_nowait())
                except queue.Empty:
                    break
            send_message(conn, COMMANDS, build_commands(frame, encoded))


def stdin_loop(pending):
    for line in sys.stdin:
        line = line.strip()
        if line in ("quit", "exit"):
            break
        if line == "help":
            print(__doc__)
            continue
        try:
            pending.put(parse_command_line(line))
        except (ValueError, IndexError) as error:
            print("error:", error)
    import os
    os._exit(0)


def selftest():
    """Plays a fake emulator against serve() and checks the replies."""
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    pending = queue.Queue()
    logs = []

    def run_server():
        conn, _ = listener.accept()
        try:
            serve(conn, pending, 1, out=logs.append)
        except ConnectionError:
            pass

    thread = threading.Thread(target=run_server, daemon=True)
    thread.start()

    emu = socket.create_connection(("127.0.0.1", port))
    sizes = {EV_STATE: 71}
    table = bytes([1]) + struct.pack("<BH", EV_STATE, 71)
    hello = struct.pack("<H", 1) + b"SmashRemix2.0.1".ljust(64, b"\0") + struct.pack("<I", 3) + table
    send_message(emu, HELLO, hello)
    msg_type, payload = recv_message(emu)
    assert msg_type == HELLO_ACK and payload == struct.pack("<HB", 1, 1), (msg_type, payload)

    pending.put(parse_command_line("damage 0 120"))
    pending.put(parse_command_line("save 2"))
    state = struct.pack("<iBBH", 0, 0, 0, 0x0A) + struct.pack("<ff", 1.5, 2.5) + b"\0" * 4 + struct.pack("<ff", 0, 0)
    state = state.ljust(0x1C, b"\0") + struct.pack("<I", 37) + struct.pack("<b", 2) + b"\0\1" + b"\0" * (71 - 0x23)
    assert len(state) == 71, len(state)
    frame_payload = struct.pack("<iBB", 0, 0, 0) + bytes([EV_STATE]) + state
    send_message(emu, FRAME, frame_payload)
    msg_type, payload = recv_message(emu)
    assert msg_type == COMMANDS
    frame, count = struct.unpack_from("<iB", payload, 0)
    assert (frame, count) == (0, 2), (frame, count)
    assert payload[5] == 0x01 and struct.unpack_from("<H", payload, 6)[0] == 41
    assert payload[5 + 3 + 41] == 0x05 and payload[5 + 3 + 41 + 3] == 2

    send_message(emu, FRAME, struct.pack("<iBB", 1, 0b01, 2) + bytes([0, 6]))  # stateLoaded, results Ok+Pending
    recv_message(emu)
    emu.close()
    thread.join(timeout=2)
    text = "\n".join(logs)
    assert "P0 char=0x00 act=0x00a pos=(1.5,2.5)" in text, text
    assert "dmg=37 stocks=2 air" in text, text
    assert "state loaded" in text and "Ok, Pending" in text, text
    print("selftest ok")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=46464)
    parser.add_argument("--every", type=int, default=30, help="print match state every N frames")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return

    pending = queue.Queue()
    threading.Thread(target=stdin_loop, args=(pending,), daemon=True).start()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(1)
    print("listening on 127.0.0.1:%d (type 'help' for commands)" % args.port)
    while True:
        conn, _ = listener.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("emulator connected")
        try:
            serve(conn, pending, args.every)
        except (ConnectionError, ValueError) as error:
            print("disconnected:", error)
        finally:
            conn.close()


if __name__ == "__main__":
    main()

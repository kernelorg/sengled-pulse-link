#!/usr/bin/env python3
"""Sengled Pulse Link (C01-BR30) protocol tool: build, send and sniff frames.

Arguments are key=value pairs. Run `python3 main.py help` for details.
"""
import sys
import time

HELP = """\
Usage: python3 main.py [key=value ...]
       python3 main.py help            show this help
       python3 main.py help=commands   list commands

Modes (mode=...):
  sniff   read frames from the serial port and decode them   (default)
  send    build a command, send it, print and decode replies
  build   only print the frame as hex, no serial port needed
  decode  decode a frame given as hex=...
  nodes   list the lamps known to the gateway
  add     add (pair) the lamp dst=... to the gateway, about 20 s
  remove  remove (unpair) the lamp dst=... from the gateway

Keys:
  mode=sniff|send|build|decode|nodes|add|remove
  port=/dev/ttyUSB0     serial port, or /dev/rfcomm0 for the Bluetooth SPP
  baud=115200           baud rate (the module UART runs at 115200)
  cmd=NAME              command for send/build, see help=commands
  dst=master            target: master (00000000, the gateway itself),
                        all (FFFFFFFF, broadcast), or a lamp address
                        as 8 hex digits big-endian, e.g. 00020056;
                        the lamp label / QR text "SN:...;AD:00020056"
                        is accepted too
  value=N               first parameter (decimal or 0x..)
  value2=N              second parameter (brightness only)
  timeout=3             seconds to wait for replies in send mode
  raw=1                 also print the raw hex of every frame
  hex="7E FE FF ..."    frame for mode=decode

Examples:
  python3 main.py                                   sniff /dev/ttyUSB0
  python3 main.py mode=build cmd=lamp_off dst=all
  python3 main.py mode=send port=/dev/rfcomm0 cmd=volume value=40 dst=all
  python3 main.py mode=send cmd=lamp_state dst=00020056
  python3 main.py mode=decode hex="7E FE FF FF FF 00 00 00 00 00 00 01 00 FC 00 7E"
  python3 main.py mode=nodes
  python3 main.py mode=add dst=00020056
  python3 main.py mode=add dst="SN:14411008EU01926;AD:00020056"
  python3 main.py mode=remove dst=0001FEB6
"""

ADDR_NAMES = {
    0xFFFFFFFE: "client",     # the phone app / Bluetooth side
    0xFFFFFFFF: "all",        # broadcast
    0x00000000: "master",     # the gateway itself
}

# (subsystem, cmd) -> (name, total frame length, payload builder, description)
COMMANDS = {
    (0, 0):  ("generic_info", 16, None, "device info: name (C01-BR30 Link), ids"),
    (0, 1):  ("handshake",    16, None, "handshake / ping a node, reply 01 = ok"),
    (0, 2):  ("enum_slaves",  16, None, "list nodes: count (incl. master) and addresses"),
    (0, 4):  ("set_name",     32, "name", "set node name, value=text up to 13 bytes"),
    (0, 5):  ("node_name",    16, None, "query node name"),
    (0, 7):  ("identify",     16, None, "config start: the lamp blinks"),
    (0, 8):  ("config_end",   16, None, "config end"),
    (0, 11): ("set_config",   20, {13: "value", 15: "value2"}, "set config item value=item value2=data (item 0 = light memory 0/1)"),
    (0, 12): ("query_config", 18, {13: "value"}, "query config item value=item"),
    (1, 0):  ("lamp_onoff",   20, {15: "value"}, "lamp on/off: value=0 on, 1 off"),
    (1, 1):  ("brightness",   20, {15: "value", 16: "value2"}, "brightness 0..100; value=group value2=own, 255 = keep (app: all -> value=N value2=255, node -> value=255 value2=N, then all 255/100 = apply)"),
    (1, 3):  ("lamp_state",   18, None, "query lamp on/off and brightness"),
    (2, 0):  ("channel",      19, {15: "value"}, "sound channel: 0 right, 1 left, 2 left+right"),
    (2, 1):  ("eq",           19, {15: "value"}, "equalizer: 0 normal 1 pop 2 jazz 3 classic 4 rock 5 movie"),
    (2, 2):  ("mute",         19, {15: "value"}, "sound: value=1 mute, 0 unmute"),
    (2, 3):  ("volume",       20, {15: "value", 16: 0xFF}, "volume 0..100"),
    (2, 4):  ("speaker_state", 18, None, "query channel, eq, mute, volume"),
    (4, 0):  ("connect_code", 16, None, "query network code (= gateway SN)"),
    (4, 1):  ("set_connect_code", 20, "code", "write network code to a lamp, value=code (0x00FFFFFF unbinds)"),
    (4, 2):  ("set_id_address", 24, "idaddr", "gateway node table: value=lamp address (hex), value2=0 add, 1 delete"),
    (4, 6):  ("audio_source", 19, {15: "value"}, "audio source: 0 3.5mm, 1 bluetooth"),
    (4, 7):  ("get_audio_source", 16, None, "query audio source"),
}
BY_NAME = {v[0]: k for k, v in COMMANDS.items()}
# aliases for convenience
ALIASES = {"lamp_on": ("lamp_onoff", 0), "lamp_off": ("lamp_onoff", 1),
           "unmute": ("mute", 0), "sound_off": ("mute", 1), "sound_on": ("mute", 0)}

CHANNELS = {0: "right", 1: "left", 2: "left+right"}
EQS = {0: "normal", 1: "pop", 2: "jazz", 3: "classic", 4: "rock", 5: "movie"}
SOURCES = {0: "3.5mm", 1: "bluetooth"}


def die(msg):
    print(f"error: {msg}\n", file=sys.stderr)
    print(HELP, file=sys.stderr)
    sys.exit(2)


def parse_args(argv):
    opts = {}
    for arg in argv:
        if arg in ("help", "-h", "--help"):
            opts["help"] = ""
            continue
        if "=" not in arg:
            die(f"argument '{arg}' is not key=value")
        k, v = arg.split("=", 1)
        opts[k.strip().lower()] = v.strip()
    known = {"help", "mode", "port", "baud", "cmd", "dst", "value", "value2",
             "timeout", "raw", "hex"}
    unknown = set(opts) - known
    if unknown:
        die("unknown key(s): " + ", ".join(sorted(unknown)))
    return opts


def num(s, what):
    try:
        return int(s, 0)
    except (TypeError, ValueError):
        die(f"{what}={s!r} is not a number")


def parse_addr(s):
    s = (s or "master").strip()
    if "AD:" in s.upper():                      # label / QR text "...;AD:00020056"
        s = s[s.upper().rindex("AD:") + 3:]
    s = s.lower()
    if s == "master":
        return 0
    if s in ("all", "broadcast"):
        return 0xFFFFFFFF
    if s.startswith("0x"):
        s = s[2:]
    try:
        return int(s, 16) & 0xFFFFFFFF
    except ValueError:
        die(f"dst={s!r}: use master, all or 8 hex digits like 00020056")


def fmt_addr(a):
    return ADDR_NAMES.get(a, f"{a:08X}")


def checksum(frame):
    return sum(frame[1:len(frame) - 3]) & 0xFF


def escape(frame):
    out = bytearray([0x7E])
    for x in frame[1:-1]:
        out += bytes([0x7D, x ^ 0x20]) if x in (0x7D, 0x7E) else bytes([x])
    out.append(0x7E)
    return bytes(out)


def unescape(body):
    out, esc = bytearray(), False
    for x in body:
        if esc:
            out.append(x ^ 0x20)
            esc = False
        elif x == 0x7D:
            esc = True
        else:
            out.append(x)
    return bytes(out)


def build(name, dst, value=None, value2=None):
    fixed = None
    if name in ALIASES:
        name, fixed = ALIASES[name]
    if name not in BY_NAME:
        die(f"unknown cmd '{name}', see help=commands")
    key = BY_NAME[name]
    _, length, payload, _ = COMMANDS[key]
    if fixed is not None:
        value = str(fixed)
    f = bytearray(length)
    f[0] = f[-1] = 0x7E
    f[1:5] = (0xFFFFFFFE).to_bytes(4, "little")
    f[5:9] = dst.to_bytes(4, "little")
    f[9], f[11] = key
    vals = {"value": value, "value2": value2}
    if isinstance(payload, dict):
        for off, src in payload.items():
            if isinstance(src, int):
                f[off] = src
            else:
                v = vals[src]
                if v is None and src == "value2" and name == "brightness":
                    v = "255"
                if v is None:
                    die(f"cmd={name} needs {src}=")
                f[off] = num(v, src) & 0xFF
    elif payload == "name":
        if value is None:
            die("cmd=set_name needs value=<name>")
        b = value.encode()[:13]
        f[13:13 + len(b)] = b
    elif payload == "code":
        if value is None:
            die("cmd=set_connect_code needs value=<code>")
        f[13:17] = (num(value, "value") & 0xFFFFFFFF).to_bytes(4, "little")
    elif payload == "idaddr":
        if value is None:
            die("cmd=set_id_address needs value=<lamp address hex>")
        f[13] = num(value2 or "0", "value2") & 0xFF
        f[17:21] = parse_addr(value).to_bytes(4, "little")
    f[length - 3] = checksum(f)
    return escape(f)


def decode(frame):
    """frame: unescaped bytes starting and ending with 0x7E."""
    if len(frame) < 15:
        return f"short frame: {frame.hex(' ')}"
    src = int.from_bytes(frame[1:5], "little")
    dst = int.from_bytes(frame[5:9], "little")
    key = (frame[9], frame[11])
    n = len(frame)
    ok = checksum(frame) == frame[n - 3]
    p = frame[13:n - 3]  # payload, without checksum / 00 7E
    name = COMMANDS.get(key, (f"unknown {key[0]}/{key[1]}",))[0]
    reply = dst == 0xFFFFFFFE
    head = f"{fmt_addr(src):>8} -> {fmt_addr(dst):<8} {name:<16}"
    info = describe(key, p, reply)
    return f"{head} {info}{'' if ok else '  [BAD CHECKSUM]'}"


def describe(key, p, reply):
    g = lambda i: p[i] if i < len(p) else None
    try:
        if key == (1, 0):
            return "on" if g(2) == 0 else "off"
        if key == (1, 1):
            a, b = g(2), g(3)
            if a == 255 and b == 100:
                return "apply"
            return f"group={a if a != 255 else '-'} own={b if b != 255 else '-'}"
        if key == (1, 3) and reply:
            return f"{'on' if g(2) == 0 else 'off'} brightness={g(3)}%"
        if key == (2, 0):
            return CHANNELS.get(g(2), g(2))
        if key == (2, 1):
            return EQS.get(g(2), g(2))
        if key == (2, 2):
            return "mute" if g(2) == 1 else "unmute"
        if key == (2, 3):
            return f"volume={g(2)}%"
        if key == (2, 4) and reply:
            return (f"channel={CHANNELS.get(g(2), g(2))} eq={EQS.get(g(3), g(3))} "
                    f"{'mute' if g(4) else 'unmute'} volume={g(5)}%")
        if key == (4, 6):
            return SOURCES.get(g(2), g(2))
        if key == (4, 7) and reply:
            return SOURCES.get(g(0), g(0))
        if key == (4, 0) and reply:
            return f"code={int.from_bytes(p[0:4], 'little'):08X}"
        if key == (4, 1):
            return f"code={int.from_bytes(p[0:4], 'little'):08X}"
        if key == (4, 2):
            op = "add" if g(0) == 0 else "delete"
            return f"{op} {int.from_bytes(p[4:8], 'little'):08X}"
        if key == (0, 1) and reply:
            return "ok" if g(0) == 1 else f"result={g(0)}"
        if key == (0, 2) and reply:
            count = int.from_bytes(p[2:4], "little")
            addrs = [int.from_bytes(p[4 + 4 * i:8 + 4 * i], "little") for i in range(count)]
            return f"nodes={count}: " + ", ".join(fmt_addr(a) for a in addrs)
        if key == (0, 0) and reply:
            return "name=" + p[:32].split(b"\0")[0].decode(errors="replace")
        if key == (0, 4):
            return "name=" + p[:13].split(b"\0")[0].decode(errors="replace")
        if key == (0, 11):
            return f"item={g(0)} value={g(2)}"
        if key == (0, 12):
            return f"item={g(0)}" + (f" value={g(2)}" if reply else "")
    except Exception:
        pass
    return p.hex(" ") if p else ""


class FrameReader:
    """Split a byte stream into 7E...7E frames; other bytes are text."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def read(self, timeout=None):
        end = None if timeout is None else time.monotonic() + timeout
        while True:
            start = self.buf.find(b"\x7e")
            if start > 0:
                text, self.buf = bytes(self.buf[:start]), self.buf[start:]
                return "text", text
            if start == 0:
                stop = self.buf.find(b"\x7e", 1)
                if stop == 1:          # 7E 7E: previous frame end + new start
                    del self.buf[0]
                    continue
                if stop > 1:
                    body, self.buf = bytes(self.buf[1:stop]), self.buf[stop + 1:]
                    return "frame", b"\x7e" + unescape(body) + b"\x7e"
            if end is not None and time.monotonic() >= end:
                return None, None
            data = self.ser.read(max(1, self.ser.in_waiting))
            self.buf += data


def print_item(kind, data, raw):
    if kind == "text":
        t = data.decode(errors="replace").strip()
        if t:
            print(f"   text: {t}")
    elif kind == "frame":
        print(decode(data))
        if raw:
            print("         " + data.hex(" ").upper())


def open_port(opts):
    try:
        import serial
    except ImportError:
        die("pyserial is required: pip install pyserial")
    return serial.Serial(opts.get("port", "/dev/ttyUSB0"),
                         num(opts.get("baud", "115200"), "baud"), timeout=0.2)


class Gateway:
    """Request/reply helper plus the app's add/remove lamp procedures
    (SLSpeakerFunction.addNewDevice / deleteDeviceItem in the app 4.2.1)."""

    SETTLE = 8.5      # the app waits 8.5 s after changing a network code
    REPLY = 3.0       # the app's reply timeout
    TRIES = 3
    UNBOUND = 0x00FFFFFF

    def __init__(self, ser, raw=False):
        self.ser = ser
        self.rd = FrameReader(ser)
        self.raw = raw

    def send(self, name, dst=0, value=None, value2=None):
        pkt = build(name, dst, None if value is None else str(value),
                    None if value2 is None else str(value2))
        if self.raw:
            print("    >> " + pkt.hex(" ").upper())
        self.ser.write(pkt)
        self.ser.flush()
        time.sleep(0.08)   # the gateway CPU is slow, keep a gap between frames

    def request(self, name, dst=0, value=None, value2=None):
        """Send and return the payload of the matching reply, or None."""
        key = BY_NAME[name]
        self.send(name, dst, value, value2)
        deadline = time.monotonic() + self.REPLY
        while (left := deadline - time.monotonic()) > 0:
            kind, f = self.rd.read(left)
            if kind is None:
                break
            if kind != "frame" or len(f) < 15:
                continue
            if self.raw:
                print("    << " + f.hex(" ").upper())
            src = int.from_bytes(f[1:5], "little")
            fdst = int.from_bytes(f[5:9], "little")
            if (f[9], f[11]) == key and src == dst and fdst == 0xFFFFFFFE \
                    and checksum(f) == f[len(f) - 3]:
                return f[13:len(f) - 3]
        return None

    def code(self):
        p = self.request("connect_code", 0)
        return None if p is None or len(p) < 4 else int.from_bytes(p[0:4], "little")

    def nodes(self):
        p = self.request("enum_slaves", 0)
        if p is None or len(p) < 4:
            return None
        count = int.from_bytes(p[2:4], "little")
        return [int.from_bytes(p[4 + 4 * i:8 + 4 * i], "little")
                for i in range(count) if 8 + 4 * i <= len(p)]

    def print_nodes(self):
        n = self.nodes()
        if n is None:
            print("no reply from gateway")
            return False
        print("gateway nodes: " + (", ".join("gateway" if a == 0 else f"{a:08X}" for a in n) or "none"))
        return True

    def add(self, lamp):
        say = lambda s: print(f"[{lamp:08X}] {s}")
        cc = self.code()
        if cc is None:
            say("no reply from gateway")
            return False
        if cc in (0, self.UNBOUND):
            # the app would invent a new code here and drop every lamp; don't
            say(f"gateway network code is {cc:08X}, fix it with the app first")
            return False
        say(f"gateway network code {cc:08X}")
        try:
            for hs in range(1, self.TRIES + 1):
                for t in range(1, self.TRIES + 1):
                    say("opening network (gateway code -> 0), waiting 8.5 s")
                    self.send("set_connect_code", 0, 0)
                    time.sleep(self.SETTLE)
                    if self.code() == 0:
                        break
                else:
                    raise RuntimeError("gateway did not accept code 0")
                for t in range(1, self.TRIES + 1):
                    say(f"writing code {cc:08X} to the lamp and back to the gateway, waiting 8.5 s")
                    self.send("set_connect_code", lamp, cc)
                    self.send("set_connect_code", 0, cc)
                    time.sleep(self.SETTLE)
                    if self.code() == cc:
                        break
                else:
                    raise RuntimeError("gateway code was not restored")
                if self.request("handshake", lamp) is not None:
                    break
                say(f"lamp does not answer the handshake (try {hs}/{self.TRIES})")
            else:
                raise RuntimeError("lamp does not answer (power it on, close to the gateway)")
            say("handshake ok, adding to the gateway table")
            self.send("set_id_address", 0, f"{lamp:08X}", 0)
            time.sleep(self.REPLY)
        except (RuntimeError, KeyboardInterrupt) as e:
            say(f"failed: {e or 'interrupted'}; restoring gateway code {cc:08X}")
            self.send("set_connect_code", 0, cc)
            self.print_nodes()
            return False
        say("added")
        self.print_nodes()
        return True

    def remove(self, lamp):
        say = lambda s: print(f"[{lamp:08X}] {s}")
        answered = any(self.request("handshake", lamp) is not None for _ in range(2))
        if answered:
            say("handshake ok, unbinding the lamp (code 00FFFFFF)")
            self.send("set_connect_code", lamp, self.UNBOUND)
        else:
            say("lamp does not answer, removing it from the gateway table only")
        self.send("set_id_address", 0, f"{lamp:08X}", 1)
        time.sleep(1)
        say("removed")
        self.print_nodes()
        return True


def print_commands():
    print("Commands (cmd=...):")
    for key, (name, _, _, desc) in sorted(COMMANDS.items()):
        print(f"  {name:<18} {key[0]}/{key[1]:<3} {desc}")
    print("Aliases: " + ", ".join(f"{a} = {n} value={v}" for a, (n, v) in ALIASES.items()))


def main():
    opts = parse_args(sys.argv[1:])
    if "help" in opts:
        if opts["help"] == "commands":
            print_commands()
        else:
            print(HELP)
        return
    mode = opts.get("mode", "sniff")
    raw = opts.get("raw", "0") not in ("0", "", "no", "false")

    if mode in ("build", "send"):
        if "cmd" not in opts:
            die(f"mode={mode} needs cmd=")
        pkt = build(opts["cmd"], parse_addr(opts.get("dst")),
                    opts.get("value"), opts.get("value2"))
        print(pkt.hex(" ").upper())
        if mode == "build":
            return
        ser = open_port(opts)
        ser.write(pkt)
        ser.flush()
        rd = FrameReader(ser)
        deadline = time.monotonic() + float(opts.get("timeout", "3"))
        got = False
        while (left := deadline - time.monotonic()) > 0:
            kind, data = rd.read(left)
            if kind is None:
                break
            got = True
            print_item(kind, data, raw)
        if not got:
            print("no reply", file=sys.stderr)
    elif mode == "decode":
        if "hex" not in opts:
            die("mode=decode needs hex=")
        try:
            b = bytes.fromhex(opts["hex"].replace("|", " "))
        except ValueError:
            die("hex= is not valid hex")
        if b[:1] != b"\x7e" or b[-1:] != b"\x7e":
            die("frame must start and end with 7E")
        print(decode(b"\x7e" + unescape(b[1:-1]) + b"\x7e"))
    elif mode == "nodes":
        if not Gateway(open_port(opts), raw).print_nodes():
            sys.exit(1)
    elif mode in ("add", "remove"):
        if "dst" not in opts:
            die(f"mode={mode} needs dst=<lamp address>")
        lamp = parse_addr(opts["dst"])
        if lamp in (0, 0xFFFFFFFF, 0xFFFFFFFE):
            die("dst= must be a lamp address, not master/all")
        gw = Gateway(open_port(opts), raw)
        ok = gw.add(lamp) if mode == "add" else gw.remove(lamp)
        sys.exit(0 if ok else 1)
    elif mode == "sniff":
        rd = FrameReader(open_port(opts))
        try:
            while True:
                print_item(*rd.read(), raw)
        except KeyboardInterrupt:
            pass
    else:
        die(f"unknown mode '{mode}'")


if __name__ == "__main__":
    main()

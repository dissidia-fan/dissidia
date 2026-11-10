#!/usr/bin/env python3
"""Protocol test for adhoc_relay: python3 test_relay.py [host] [port]  (a relay must be running)."""
import socket, struct, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 27500
T_BEACON, T_PDP, T_MATCH, T_PTP, T_RELAY, T_FRAG = 1, 2, 3, 4, 5, 6
BCAST = b"\xff" * 6

def pkt(ptype, mac, body):
    return b"DSAH" + bytes([ptype]) + mac + struct.pack("<H", len(body)) + body

class Client:
    def __init__(self, mac, room):
        self.mac, self.room = mac, room
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(0.3)
        self.srv = (HOST, PORT)
    def hello(self, cookie=0):
        body = bytes([1]) + self.room.encode().ljust(32, b"\0") + struct.pack("<Q", cookie)
        self.s.sendto(pkt(T_RELAY, self.mac, body), self.srv)
    def recv(self):
        try:
            d, _ = self.s.recvfrom(65536)
            return d
        except socket.timeout:
            return None
    def register(self):
        self.hello(0)
        d = self.recv()
        assert d and d[4] == T_RELAY and d[HDR] == 2, "expected CHALLENGE, got %r" % d
        cookie = struct.unpack("<Q", d[HDR + 1:HDR + 9])[0]
        self.hello(cookie)
        d = self.recv()
        assert d and d[4] == T_RELAY and d[HDR] == 3, "expected OK, got %r" % d
        return struct.unpack("<H", d[HDR + 1:HDR + 3])[0]
    def send(self, ptype, body):
        self.s.sendto(pkt(ptype, self.mac, body), self.srv)

HDR = 13
fails = 0
def check(name, cond):
    global fails
    print(("ok   " if cond else "FAIL ") + name)
    if not cond: fails += 1

ma, mb, mc, md = (bytes([2, 0, 0, 0, 0, i]) for i in (1, 2, 3, 4))
room = "t%d" % int(time.time())
a, b, c = Client(ma, room), Client(mb, room), Client(mc, room + "x")
check("A registers (1 in room)", a.register() == 1)
check("B registers (2 in room)", b.register() == 2)
check("C registers in another room", c.register() == 1)

a.send(T_BEACON, b"beacon-A")
d = b.recv(); check("beacon from A reaches B", d is not None and d[4] == T_BEACON and d.endswith(b"beacon-A"))
check("beacon not seen by A itself", a.recv() is None)
check("beacon not seen in the other room", c.recv() is None)

a.send(T_MATCH, mb + b"hello-B")
d = b.recv(); check("directed packet reaches B", d is not None and d[4] == T_MATCH and d.endswith(b"hello-B"))
check("directed packet not seen by C", c.recv() is None)

a.send(T_FRAG, mb + struct.pack('<IBBB', 7, 0, 2, T_MATCH) + b"fragment-0")
d = b.recv(); check("fragment is routed by destination MAC", d is not None and d[4] == T_FRAG and d.endswith(b"fragment-0"))
check("fragment not seen by C", c.recv() is None)

a.send(T_PDP, BCAST + b"to-all")
d = b.recv(); check("broadcast reaches B", d is not None and d.endswith(b"to-all"))
check("broadcast not seen by A", a.recv() is None)

a.send(T_PTP, mc + b"cross-room")
check("packet to a MAC in another room is dropped", c.recv() is None)

# Spoofed MAC from B's socket claiming to be A
spoof = pkt(T_MATCH, ma, mb + b"spoof")
b.s.sendto(spoof, b.srv)
check("spoofed source MAC is dropped", b.recv() is None)

# Unregistered sender is ignored
x = Client(md, room)
x.send(T_BEACON, b"nope")
check("unregistered sender is ignored", b.recv() is None)

# Bad cookie gets a fresh challenge, not registration
y = Client(md, room)
y.hello(0x1234567890abcdef)
d = y.recv(); check("wrong cookie is challenged again", d is not None and d[HDR] == 2)

# Keep-alive keeps the count, a second hello from A does not duplicate it
check("A keep-alive", a.register() == 2)

# Bye removes the player
a.s.sendto(pkt(T_RELAY, ma, bytes([4])), a.srv)
time.sleep(0.2)
b2 = Client(mb, room)
check("after A's bye, the room has one player", b2.register() == 1)

print("PASSED" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)

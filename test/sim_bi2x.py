#!/usr/bin/env python3
"""
Simulated BIO2/BI2X board for testing the host stack without hardware.

Implements the device side of the IOB2 framing and the command set the host
uses (sync, enumerate, firmware info, module upload, TDJ I/O, tape LEDs).
It deliberately stresses the host: random frame encodings, encryption after
module start, junk bytes, keep-alive / notification frames and dropped
responses.

usage: sim_bi2x.py <tdj module .bin> <sci module .bin> [--seconds N]
Prints the pty path on the first line of stdout, then a JSON report when done.
"""
import json
import os
import random
import select
import sys
import time
import tty

rng = random.Random(1234)


def crc_reflected(crc, poly, data):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
    return crc


def crc_normal(crc, poly, width, data):
    top = 1 << (width - 1)
    mask = (1 << width) - 1
    for b in data:
        for bit in range(7, -1, -1):
            fb = (1 if crc & top else 0) ^ ((b >> bit) & 1)
            crc = (crc << 1) & mask
            if fb:
                crc ^= poly
    return crc


# SIM_CRC=msb makes the simulated board use MSB-first CRCs (tests probing)
MSB = os.environ.get('SIM_CRC') == 'msb'


def crc4(crc, data):
    if MSB:
        return crc_normal(crc & 0xF, 0x03, 4, data)
    return crc_reflected(crc & 0xF, 0x0C, data) & 0xF


def crc7(crc, data):
    if MSB:
        return crc_normal(crc & 0x7F, 0x09, 7, data)
    return crc_reflected(crc & 0x7F, 0x48, data) & 0x7F


def cipher(state, b):
    if (~b & 0xAA) & 0xFF == 0:
        return state, b
    state = (state * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
    mask = 0x55 if b & 0x80 else 0x7F
    return state, b ^ (state & mask)


def head_sum(addr, tag, count, length, flags):
    c = crc4(0xF, [addr])
    c = crc4(c, [tag])
    if 2 <= count <= 5:
        if count >= 5:
            c = crc4(c, [((length >> 25) | 0xC0) & 0xFF])
        if count >= 4:
            c = crc4(c, [((length >> 19) | 0xC0) & 0xFF])
        if count >= 3:
            c = crc4(c, [((length >> 13) | 0xC0) & 0xFF])
        c = crc4(c, [((length >> 7) | 0xC0) & 0xFF])
    c = crc4(c, [length & 0x7F])
    c = crc4(c, [flags & 0xF0])
    return c ^ 0xF


def encode(node, response, tag, data, mode=None, encrypt=False):
    data = bytes(data)
    out = bytearray([0xAA, ((node & 0x3F) << 1) | (1 if response else 0), tag])
    n = len(data)
    for shift in (25, 19, 13, 7):
        g = (n >> shift) & 0x3F
        if g:
            out.append(0xC0 | g)
    out.append(n & 0x7F)
    count = len(out) - 3
    if mode is None:
        if n == 0:
            mode = 0
        else:
            choices = [0, 3]
            if 0xAA not in data:
                choices.append(2)
            mode = rng.choice(choices)
    if mode == 3 and n:
        free = [v for v in range(256) if v not in data]
        if not free:
            mode = 0
    if mode == 2 and 0xAA in data:
        mode = 0
    if n == 0:
        encrypt = False
    flags = (mode << 5) | (0x10 if encrypt else 0)
    flags |= head_sum(out[1], tag, count, n, flags)
    out.append(flags)
    body = bytearray()
    if n:
        if mode == 0:
            for b in data:
                if b in (0xAA, 0xFF):
                    body += bytes([0xFF, (~b) & 0xFF])
                else:
                    body.append(b)
        elif mode == 3:
            subst = rng.choice([v for v in range(256) if v not in data])
            body.append(subst)
            body += bytes(subst if b == 0xAA else b for b in data)
        else:
            body += data
        body.append(crc7(0x7F, data) ^ 0x7F)
        if encrypt:
            st = tag ^ 0xAA  # device -> host keystream seed
            enc = bytearray()
            for b in body:
                st, e = cipher(st, b)
                enc.append(e)
            body = enc
    return bytes(out + body)


class Decoder:
    """Device-side decoder for host frames."""

    def __init__(self, sim):
        self.sim = sim
        self.state = 'idle'

    def feed(self, raw):
        if raw == 0xAA:
            self.state = 'addr'
            self.dec = None
            return None
        b = raw
        if self.dec is not None:
            self.dec_state, b = cipher(self.dec_state, raw)
        st = self.state
        if st == 'idle':
            return None
        if st == 'addr':
            self.addr = b
            self.state = 'tag'
        elif st == 'tag':
            self.tag = b
            self.len = 0
            self.count = 0
            self.state = 'len'
        elif st == 'len':
            self.count += 1
            if b & 0x80:
                if not (b & 0x40) or self.count > 4:
                    self.sim.errors.append('bad length byte')
                    self.state = 'idle'
                    return None
                self.len = (self.len << 6) | (b & 0x3F)
            else:
                self.len = (self.len << 7) | (b & 0x7F)
                self.state = 'flags'
        elif st == 'flags':
            if (b & 0xF) != head_sum(self.addr, self.tag, self.count, self.len, b):
                self.sim.errors.append('bad header crc from host')
                self.state = 'idle'
                return None
            self.mode = b >> 5
            if self.mode > 3:
                self.sim.errors.append('host used mode %d' % self.mode)
                self.state = 'idle'
                return None
            self.enc = bool(b & 0x10)
            self.node = (self.addr >> 1) & 0x3F
            if self.enc:
                if not self.sim.encrypting.get(self.node):
                    self.sim.errors.append('host encrypted before device did')
                self.dec = True
                self.dec_state = self.tag ^ 0x55  # host -> device seed
                self.sim.host_encrypted_frames += 1
            elif self.sim.encrypting.get(self.node) and self.len:
                self.sim.errors.append('host sent plain frame after encryption started')
            self.data = bytearray()
            self.esc = False
            if self.len == 0:
                self.state = 'idle'
                return (self.node, self.addr & 1, self.tag, b'')
            self.state = 'subst' if self.mode == 3 else 'data'
        elif st == 'subst':
            self.subst = b
            self.state = 'data'
        elif st == 'data':
            if self.mode == 0:
                if self.esc:
                    b = (~b) & 0xFF
                    self.esc = False
                elif b == 0xFF:
                    self.esc = True
                    return None
            elif self.mode == 3 and b == self.subst:
                b = 0xAA
            self.data.append(b)
            if len(self.data) >= self.len:
                self.state = 'crc'
        elif st == 'crc':
            self.state = 'idle'
            if b != crc7(0x7F, self.data) ^ 0x7F:
                self.sim.errors.append('bad data crc from host')
                return None
            return (self.node, self.addr & 1, self.tag, bytes(self.data))
        return None


TAPE_BASE = [0, 38, 83, 128, 149, 279, 313, 510, 707]


class Sim:
    def __init__(self, fd, modules):
        self.fd = fd
        self.modules = modules  # expected upload order
        self.errors = []
        self.encrypting = {}
        self.host_encrypted_frames = 0
        self.dec = Decoder(self)
        self.slots = {}
        self.next_slot = 5
        self.running = {}  # id -> module name
        self.tdj_id = None
        self.loaded = []
        self.outputs = None
        self.output_writes = 0
        self.resets = []
        self.gamma = None
        self.tape = [0] * 707
        self.tape_written = set()
        self.commits = 0
        self.polls = 0
        self.dropped = 0
        self.frames_in = 0
        self.t0 = time.time()

    def write(self, b):
        os.write(self.fd, b)

    def reply(self, node, tag, data):
        enc = bool(self.encrypting.get(node))
        self.write(encode(node, True, tag, data, encrypt=enc))

    def noise(self):
        r = rng.random()
        if r < 0.05:
            self.write(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 6))
                             if True).replace(b'\xaa', b'\x00'))
        elif r < 0.08:
            self.write(encode(1, True, 0x42, b'\x01\x02', mode=1))
        elif r < 0.10:
            self.write(encode(1, True, 0x43, b'\x00\x7f\x05'))

    def status_bytes(self):
        t = time.time() - self.t0
        raw = bytearray(0x4A)
        raw[1] = 0b0101 if int(t * 2) % 2 else 0  # test + coin blink
        raw[2] = 0b1001  # 1P start + EFFECT
        raw[4] = int(t * 50) & 0xFF  # 1P turntable spinning
        raw[5] = 0x80
        raw[8] = 0b1010101  # 1P keys 1,3,5,7
        raw[9] = 0b0000010  # 2P key 2
        for i in range(10, 0x4A):
            raw[i] = i
        return bytes(raw)

    def handle(self, node, resp_bit, tag, data):
        self.frames_in += 1
        if resp_bit:
            self.errors.append('host set response bit')
        if node == 0 and not data:
            self.write(encode(0, False, tag, b''))
            return
        if node == 0 and data[:2] == b'\x00\x01':
            self.reply(0, tag, b'\x00\x01\x01')
            return
        if node != 1:
            self.errors.append('frame for unknown node %d' % node)
            return

        # occasionally lose a response (host must retry)
        if self.frames_in > 20 and rng.random() < 0.03:
            self.dropped += 1
            return

        if data[0] == 0x00:
            cmd = data[1]
            if cmd == 0x02:
                fi = bytearray(32)
                fi[0:4] = b'\x0d\x06\x00\x01'
                fi[5:7] = b'\x01\x02'
                fi[7] = 5
                fi[8:12] = b'BI2X'
                fi[0x10:0x1E] = bytes([0, 0, 1, 0x0B, 0, 0, 0, 0, 0, 0, 0, 0, 0x4A, 0x12])
                fi[0x1E:0x20] = b'\x01\x05'
                self.reply(1, tag, b'\x00\x02\x00' + bytes(fi))
            elif cmd == 0x10:
                size = int.from_bytes(data[2:6], 'big')
                slot = self.next_slot
                self.next_slot += 1
                self.slots[slot] = bytearray(size)
                self.reply(1, tag, bytes([0, 0x10, 0, slot]))
            elif cmd == 0x13:
                slot = data[2]
                off = int.from_bytes(data[3:7], 'big')
                chunk = data[7:]
                if len(chunk) > 0x40:
                    self.errors.append('chunk too big')
                self.slots[slot][off:off + len(chunk)] = chunk
                self.reply(1, tag, bytes([0, 0x13, 0]))
            elif cmd == 0x78:
                slot = data[2]
                blob = bytes(self.slots[slot])
                idx = len(self.loaded)
                name, expect = self.modules[idx] if idx < len(self.modules) else ('?', b'')
                if blob != expect:
                    self.errors.append('module %s content mismatch' % name)
                self.loaded.append(name)
                mid = 0x40 + idx
                self.running[mid] = name
                if name == 'tdj':
                    self.tdj_id = mid
                    self.encrypting[1] = True  # start encrypting from now on
                self.reply(1, tag, bytes([0, 0x78, 0, mid]))
            else:
                self.errors.append('unknown system cmd %02x' % cmd)
            return

        # TDJ module traffic: a sequence of sub-commands
        if data[0] != self.tdj_id:
            self.errors.append('I/O frame for module %02x' % data[0])
            return
        out = bytearray()
        i = 0
        while i < len(data):
            mid, cmd = data[i], data[i + 1]
            if mid != self.tdj_id:
                self.errors.append('mixed module ids')
                return
            if cmd == 0x10:
                self.polls += 1
                out += bytes([mid, 0x10, 0]) + self.status_bytes()
                i += 2
            elif cmd == 0x11:
                self.outputs = bytes(data[i + 2:i + 19])
                self.output_writes += 1
                out += bytes([mid, 0x11, 0])
                i += 19
            elif cmd == 0x12:
                self.resets.append(int.from_bytes(data[i + 2:i + 6], 'big'))
                out += bytes([mid, 0x12, 0])
                i += 6
            elif cmd == 0x22:
                self.commits += 1
                out += bytes([mid, 0x22, 0])
                i += 2
            elif cmd == 0x20:
                self.gamma = bytes(data[i + 2:i + 258])
                out += bytes([mid, 0x20, 0])
                i += 258
            elif cmd == 0x21:
                strip = data[i + 2]
                start = int.from_bytes(data[i + 3:i + 5], 'big')
                count = int.from_bytes(data[i + 5:i + 7], 'big')
                px = data[i + 7:i + 7 + count * 2]
                for k in range(count):
                    g = TAPE_BASE[strip] + start + k
                    self.tape[g] = px[2 * k] | (px[2 * k + 1] << 8)
                    self.tape_written.add(g)
                out += bytes([mid, 0x21, 0])
                i += 7 + count * 2
            else:
                self.errors.append('unknown TDJ cmd %02x' % cmd)
                return
        self.reply(1, tag, bytes(out))

    def power_cycle(self):
        self.encrypting = {}
        self.slots = {}
        self.running = {}
        self.tdj_id = None
        self.loaded = []
        self.dec = Decoder(self)
        self.power_cycles += 1

    def run(self, seconds, blackout=None):
        end = time.time() + seconds
        self.power_cycles = 0
        while time.time() < end:
            if blackout and blackout[0] <= time.time() - self.t0 < blackout[0] + blackout[1]:
                try:
                    os.read(self.fd, 4096)
                except OSError:
                    pass
                if not getattr(self, 'in_blackout', False):
                    self.in_blackout = True
                time.sleep(0.01)
                continue
            if getattr(self, 'in_blackout', False):
                self.in_blackout = False
                self.power_cycle()
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                buf = os.read(self.fd, 4096)
            except OSError:
                break
            for b in buf:
                pkt = self.dec.feed(b)
                if pkt is not None:
                    self.noise()
                    self.handle(*pkt)


def main():
    tdj = open(sys.argv[1], 'rb').read()
    sci = open(sys.argv[2], 'rb').read()
    seconds = float(sys.argv[sys.argv.index('--seconds') + 1]) if '--seconds' in sys.argv else 8
    master, slave = os.openpty()
    tty.setraw(master)
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    sim = Sim(master, [('tdj', tdj), ('sci', sci)])
    blackout = None
    if '--blackout' in sys.argv:
        a = sys.argv[sys.argv.index('--blackout') + 1].split(',')
        blackout = (float(a[0]), float(a[1]))
    sim.run(seconds, blackout)
    report = {
        'errors': sim.errors[:20],
        'error_count': len(sim.errors),
        'loaded': sim.loaded,
        'power_cycles': sim.power_cycles,
        'outputs': sim.outputs.hex() if sim.outputs else None,
        'output_writes': sim.output_writes,
        'resets': sim.resets,
        'gamma_ok': sim.gamma is not None,
        'gamma_head': sim.gamma[:8].hex() if sim.gamma else None,
        'tape_written': len(sim.tape_written),
        'tape_sample': {str(k): sim.tape[k] for k in (0, 18, 19, 37, 38, 100, 706)},
        'commits': sim.commits,
        'polls': sim.polls,
        'dropped': sim.dropped,
        'host_encrypted_frames': sim.host_encrypted_frames,
    }
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()

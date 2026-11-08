"""Test-only independent 802.11/WPA2 peer for the PCIe laboratory device.

Frames cross a localhost UDP socket. Crypto comes from Python's independent
hashlib/hmac and cryptography implementations. Addresses and services below
belong to this fixture and are never included in the guest image.
"""

import hashlib
import hmac
import socket
import struct
import threading

from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.keywrap import aes_key_wrap

AP = bytes.fromhex("020000000001")
STA = bytes.fromhex("020000000002")
ROUTER = bytes([192, 168, 50, 1])
CLIENT = bytes([192, 168, 50, 9])
SSID = b"Luna-Lab-WPA2"
RSN = bytes.fromhex("30140100000fac040100000fac040100000fac020000")
LLC = bytes.fromhex("aaaa03000000")


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def aad(frame):
    return bytes([frame[0] & 0x8F, frame[1] & 0xC7]) + frame[4:22] + bytes([frame[22] & 15, 0])


class AccessPoint:
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.1)
        self.port = self.socket.getsockname()[1]
        self.address = None
        self.sequence = self.pn = self.rx_pn = 0
        self.authorized = False
        self.ptk = None
        self.anonce = bytes(range(1, 33))
        self.reject_auth = 1
        self.errors = []
        self.events = []
        self.connections = {}
        self.duplicate_next = False
        self.css = b"/*" + b" " * 350000 + b"*/\np { color: #123456; }"
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def close(self):
        self.stopped.set()
        self.thread.join(timeout=2)
        self.socket.close()

    def header(self, subtype, data=False, protected=False):
        self.sequence = (self.sequence + 1) & 4095
        flags = (2 if data else 0) | (0x40 if protected else 0)
        return bytes([subtype, flags, 0, 0]) + STA + AP + AP + struct.pack("<H", self.sequence << 4)

    def send(self, frame):
        self.socket.sendto(b"\1" + frame, self.address)

    def data(self, protocol, payload, encrypted=True):
        body = LLC + struct.pack("!H", protocol) + payload
        frame = self.header(8, data=True, protected=encrypted)
        if encrypted:
            self.pn += 1
            pn = self.pn.to_bytes(6, "little")
            iv = pn[:2] + b"\0\x20" + pn[2:]
            nonce = b"\0" + AP + self.pn.to_bytes(6, "big")
            frame += iv + AESCCM(self.ptk[32:48], tag_length=8).encrypt(nonce, body, aad(frame))
        else:
            frame += body
        self.send(frame)
        if encrypted and self.duplicate_next:
            self.duplicate_next = False
            retry = bytearray(frame)
            retry[1] |= 8
            self.send(retry)
            self.events.append("encrypted-retry")

    def eapol(self, info, replay, key_data=b"", mic=False):
        packet = bytearray(99 + len(key_data))
        packet[:5] = b"\2\3" + struct.pack("!H", len(packet) - 4) + b"\2"
        struct.pack_into("!HHQ", packet, 5, info, 16, replay)
        packet[17:49] = self.anonce
        struct.pack_into("!H", packet, 97, len(key_data))
        packet[99:] = key_data
        if mic:
            packet[81:97] = hmac.digest(self.ptk[:16], packet, "sha1")[:16]
        self.data(0x888E, packet, encrypted=False)

    def handshake(self, packet):
        info = struct.unpack_from("!H", packet, 5)[0]
        if info == 0x010A:
            pmk = hashlib.pbkdf2_hmac("sha1", b"password", SSID, 4096, 32)
            snonce = packet[17:49]
            ordered = (
                min(AP, STA) + max(AP, STA) + min(self.anonce, snonce) + max(self.anonce, snonce)
            )
            prefix = b"Pairwise key expansion\0" + ordered
            self.ptk = b"".join(hmac.digest(pmk, prefix + bytes([i]), "sha1") for i in range(3))[
                :48
            ]
            unsigned = bytearray(packet)
            unsigned[81:97] = b"\0" * 16
            if not hmac.compare_digest(
                packet[81:97], hmac.digest(self.ptk[:16], unsigned, "sha1")[:16]
            ):
                self.events.append("bad-password")
                return
            gtk = bytes.fromhex("dd16000fac010100") + bytes(range(16))
            wrapped = aes_key_wrap(self.ptk[16:32], RSN + gtk + b"\xdd\0")
            self.eapol(0x13CA, 2, wrapped, mic=True)
        elif info == 0x030A:
            unsigned = bytearray(packet)
            unsigned[81:97] = b"\0" * 16
            assert hmac.compare_digest(
                packet[81:97], hmac.digest(self.ptk[:16], unsigned, "sha1")[:16]
            )
            self.authorized = True
            self.duplicate_next = True
            self.events.append("WPA2-authorized")

    def ipv4(self, protocol, payload, source=ROUTER, destination=CLIENT):
        ip = bytearray(
            b"\x45\0"
            + struct.pack("!H", 20 + len(payload))
            + b"\0" * 4
            + bytes([64, protocol])
            + b"\0\0"
            + source
            + destination
        )
        struct.pack_into("!H", ip, 10, checksum(ip))
        self.data(0x800, ip + payload)

    def udp(self, source_port, dest_port, payload, source=ROUTER, destination=CLIENT):
        packet = struct.pack("!HHHH", source_port, dest_port, 8 + len(payload), 0) + payload
        self.ipv4(17, packet, source, destination)

    def dhcp(self, packet):
        options = packet[240:]
        kind = None
        index = 0
        while index < len(options):
            option = options[index]
            index += 1
            if option == 255:
                break
            if not option:
                continue
            length = options[index]
            index += 1
            if option == 53:
                kind = options[index]
            index += length
        if kind not in (1, 3):
            return
        reply = bytearray(240)
        reply[:3] = b"\2\1\6"
        reply[4:8] = packet[4:8]
        reply[16:20] = CLIENT
        reply[28:34] = STA
        reply[236:240] = bytes.fromhex("63825363")
        reply += bytes([53, 1, 2 if kind == 1 else 5, 54, 4]) + ROUTER
        reply += b"\1\4\xff\xff\xff\0\3\4" + ROUTER + b"\6\4" + ROUTER
        reply += b"\x33\4" + struct.pack("!I", 3600) + b"\xff"
        self.udp(67, 68, reply.ljust(300, b"\0"), destination=b"\xff" * 4)
        self.events.append("DHCP-offer" if kind == 1 else "DHCP-ack")

    def tcp_send(self, port, state, flags, payload=b"", options=b""):
        packet = bytearray(
            struct.pack(
                "!HHIIBBHHH",
                80,
                port,
                state["seq"],
                state["ack"],
                ((20 + len(options)) // 4) << 4,
                flags,
                4096,
                0,
                0,
            )
            + options
            + payload
        )
        pseudo = ROUTER + CLIENT + b"\0\6" + struct.pack("!H", len(packet))
        struct.pack_into("!H", packet, 16, checksum(pseudo + packet))
        self.ipv4(6, packet)
        state["seq"] += len(payload) + bool(flags & 3)

    def tcp_next(self, port, state):
        pending = state.get("response", b"")
        if pending:
            payload = pending[:512]
            state["response"] = pending[512:]
            self.tcp_send(port, state, 0x18 if state["response"] else 0x19, payload)

    def tcp(self, packet):
        port, destination, seq, ack = struct.unpack_from("!HHII", packet)
        if destination != 80:
            return
        flags = packet[13]
        if flags & 2:
            state = {"seq": 1000 + len(self.connections) * 10000, "ack": seq + 1, "request": b""}
            self.connections[port] = state
            self.tcp_send(port, state, 0x12, options=b"\2\4\4\xb0")
            return
        state = self.connections.get(port)
        if state is None:
            return
        payload = packet[(packet[12] >> 4) * 4 :]
        if payload and seq == state["ack"]:
            state["ack"] += len(payload)
            state["request"] += payload
            self.tcp_send(port, state, 0x10)
            if b"\r\n\r\n" in state["request"]:
                path = state["request"].split(b" ")[1]
                body = (
                    self.css
                    if path == b"/style.css"
                    else b'<html><head><link rel="stylesheet" href="/style.css"></head><body><p>PCIe Wi-Fi works</p></body></html>'
                )
                state["response"] = (
                    b"HTTP/1.0 200 OK\r\nContent-Length: "
                    + str(len(body)).encode()
                    + b"\r\n\r\n"
                    + body
                )
                self.events.append("HTTP " + path.decode())
                self.tcp_next(port, state)
        elif flags & 0x10 and ack == state["seq"]:
            self.tcp_next(port, state)
        if flags & 1:
            state["ack"] = seq + len(payload) + 1
            self.tcp_send(port, state, 0x10)

    def ethernet(self, protocol, packet):
        if protocol == 0x806:
            if packet[6:8] == b"\0\1":
                reply = packet[:6] + b"\0\2" + AP + ROUTER + STA + CLIENT
                self.data(0x806, reply)
            return
        if protocol != 0x800:
            return
        header = (packet[0] & 15) * 4
        assert checksum(packet[:header]) == 0
        payload = packet[header : struct.unpack_from("!H", packet, 2)[0]]
        if packet[9] == 1:
            reply = bytearray(payload)
            reply[0] = 0
            reply[2:4] = b"\0\0"
            struct.pack_into("!H", reply, 2, checksum(reply))
            self.ipv4(1, reply, packet[16:20])
        elif packet[9] == 6:
            pseudo = packet[12:20] + b"\0\6" + struct.pack("!H", len(payload))
            assert checksum(pseudo + payload) == 0
            self.tcp(payload)
        elif packet[9] == 17:
            src, dst = struct.unpack_from("!HH", payload)
            if dst == 67:
                self.dhcp(payload[8:])
            elif dst == 53:
                query = payload[8:]
                reply = query[:2] + b"\x81\x80" + query[4:6] + b"\0\1\0\0\0\0" + query[12:]
                reply += b"\xc0\x0c\0\1\0\1" + struct.pack("!IH", 60, 4) + ROUTER
                self.udp(53, src, reply, packet[16:20])

    def handle(self, frame):
        if frame[0] == 0xB0:
            self.authorized = False
            self.pn = self.rx_pn = 0
            status = 13 if self.reject_auth else 0
            self.reject_auth = max(0, self.reject_auth - 1)
            self.send(self.header(0xB0) + struct.pack("<HHH", 0, 2, status))
            self.events.append("auth-rejected" if status else "auth-accepted")
        elif frame[0] == 0:
            self.send(self.header(0x10) + struct.pack("<HHH", 0x11, 0, 0xC001))
            self.eapol(0x008A, 1)
        elif frame[0] == 8:
            payload = frame[24:]
            if frame[1] & 0x40:
                assert self.authorized
                pn = int.from_bytes(payload[:2] + payload[4:8], "little")
                assert pn > self.rx_pn
                nonce = b"\0" + STA + pn.to_bytes(6, "big")
                payload = AESCCM(self.ptk[32:48], tag_length=8).decrypt(
                    nonce, payload[8:], aad(frame)
                )
                self.rx_pn = pn
            assert payload[:6] == LLC
            protocol = struct.unpack_from("!H", payload, 6)[0]
            if protocol == 0x888E:
                self.handshake(payload[8:])
            else:
                assert self.authorized and frame[1] & 0x40
                self.ethernet(protocol, payload[8:])

    def run(self):
        try:
            while not self.stopped.is_set():
                try:
                    packet, self.address = self.socket.recvfrom(2400)
                except socket.timeout:
                    continue
                if packet[0] != 1:
                    continue
                if len(packet) == 1:
                    beacon = b"\x80\0\0\0" + b"\xff" * 6 + AP + AP + b"\0\0" + b"\0" * 8
                    beacon += struct.pack("<HH", 100, 0x11) + bytes([0, len(SSID)]) + SSID
                    self.send(beacon + b"\1\4\x82\x84\x8b\x96\3\1\1" + RSN)
                else:
                    self.handle(packet[1:])
        except Exception as error:
            self.errors.append(repr(error))

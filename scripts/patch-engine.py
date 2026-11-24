# Applies nekodrop's changes to the staged copy of winawdl's AirDrop engine.
# Usage: python patch-engine.py <staged airdrop folder>
#
# - A stable mDNS instance name (NEKODROP_SERVICE_ID, kept by the client), so
#   an engine restart (a new awdl0 after the radio restarts) shows up on the
#   iPhone as the same device instead of a second one.
# - Spare receiver ports: when 8771 is held by another program the receiver
#   takes the next free one of 8772-8779 (advertised in the mDNS SRV record,
#   so the iPhone follows), instead of waiting for 8771 forever.
# - Sender presents no TLS client certificate.  An iPhone rejects a self-signed
#   one with SSLV3_ALERT_BAD_CERTIFICATE on both /Discover and /Ask; AirDrop
#   receivers accept connections without a client cert, and identity rides in
#   the plist record instead.  Without this the send direction cannot work.
import pathlib
import sys

NL = chr(10)
HELPER = NL.join([
    'SPARE_PORTS = range(8771, 8780)  # nekodrop: the firewall rule allows these',
    '',
    '',
    'def _bind_spare(settings, watcher):',
    '    ports = [settings.port] + [p for p in SPARE_PORTS if p != settings.port]',
    '    for port in ports:',
    '        try:',
    '            return receiver.Receiver(port, settings, watcher)',
    '        except OSError as error:',
    '            if error.errno != errno.EADDRINUSE or port == ports[-1]:',
    '                raise',
    "            logging.warning('port %d held, trying the next', port)",
    '',
    '',
    '',
])

engine = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine.read_text(encoding='utf-8')
changed = False

old = 'service_id = secrets.token_hex(6)'
new = "service_id = os.environ.get('NEKODROP_SERVICE_ID') or secrets.token_hex(6)"
if new not in text:
    if old not in text:
        sys.exit('engine.py: the service id line moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    if NL + 'import os' + NL not in text:
        text = text.replace(NL + 'import secrets' + NL, NL + 'import os' + NL + 'import secrets' + NL, 1)
    changed = True

old = 'server = receiver.Receiver(self.settings.port, self.settings, self.watcher)'
new = 'server = _bind_spare(self.settings, self.watcher)'
if new not in text:
    if old not in text:
        sys.exit('engine.py: the receiver bind line moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    text = text.replace(NL + 'class ', NL + HELPER + 'class ', 1)
    changed = True

if changed:
    engine.write_text(text, encoding='utf-8')
print('engine patched')

# sender.py: drop the client certificate from the TLS context.  The receiver
# rejects our self-signed cert; a sender without an Apple-issued identity is
# expected to present none at all.
sender = pathlib.Path(sys.argv[1]) / 'sender.py'
text = sender.read_text(encoding='utf-8')
old = NL.join([
    '    certificate, key = identity.ensure_self_signed(keys_dir)',
    '    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)',
    '    # A receiver visible to everyone presents a certificate nothing vouches',
    '    # for, and takes ours the same way.',
    '    context.check_hostname = False',
    '    context.verify_mode = ssl.CERT_NONE',
    '    context.load_cert_chain(certificate, key)',
])
new = NL.join([
    '    # Present no client certificate.  An iPhone in Everyone mode rejects a',
    '    # self-signed one with SSLV3_ALERT_BAD_CERTIFICATE on every connection.',
    '    # AirDrop receivers accept a connection with no client cert; identity',
    '    # is carried in the plist record, not in the TLS client certificate.',
    '    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)',
    '    context.check_hostname = False',
    '    context.verify_mode = ssl.CERT_NONE',
])
if new not in text:
    if old not in text:
        sys.exit('sender.py: the client_context body moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    sender.write_text(text, encoding='utf-8')
    print('sender patched')

# engine.py: follow a receiver that announces itself again.  After a transfer
# in either direction an iPhone withdraws its AirDrop service and comes back
# under a new instance (#4: peer_gone, then WinError 10061 on its old port,
# then every retry failed at once with "gone").  A send to a peer that is gone,
# or refused on its old port, now waits up to FOLLOW_SECONDS for a receiver
# with the same name and sends there.
FOLLOW = [
    (
        '        self._shy_said = 0' + NL,
        '        self._shy_said = 0' + NL +
        '        self._gone_names = {}  # nekodrop: key -> name of receivers that went away' + NL,
    ),
    (
        NL.join([
            '            for key in [key for key in self._peers if key not in located]:',
            '                if self._peers.pop(key).name:',
            '                    gone.append(key)',
        ]),
        NL.join([
            '            for key in [key for key in self._peers if key not in located]:',
            '                old = self._peers.pop(key)',
            '                if old.name:',
            '                    self._gone_names[key] = old.name',
            '                    gone.append(key)',
        ]),
    ),
    (
        '    def link(self, scope):' + NL,
        NL.join([
            '    def find(self, key, wait):',
            '        """nekodrop: the receiver under `key`, or the one that came back with',
            '        its name, waiting up to `wait` seconds for it."""',
            '        deadline = time.monotonic() + wait',
            '        while True:',
            '            with self._lock:',
            '                peer = self._peers.get(key)',
            '                if peer and peer.name:',
            '                    return peer',
            '                name = self._gone_names.get(key)',
            '                if name:',
            '                    for other in self._peers.values():',
            '                        if other.name == name:',
            '                            return other',
            '            if time.monotonic() >= deadline or not self.wanted.is_set():',
            '                return None',
            '            time.sleep(0.5)',
            '',
            '    def went_away(self, key):',
            '        with self._lock:',
            '            return key in self._gone_names',
            '',
            '    def link(self, scope):',
            '',
        ]),
    ),
    (
        NL.join([
            '        peer = self.looking.peer(peer_key)',
            '        if not peer or scope is None:',
            "            failed('gone')",
            '            return',
        ]),
        NL.join([
            '        peer = self.looking.peer(peer_key)',
            '        # None for a receiver that went away: looked for again in _run.',
            '        if (not peer and not self.looking.went_away(peer_key)) or scope is None:',
            "            failed('gone')",
            '            return',
        ]),
    ),
    (
        NL.join([
            '        watch = SendWatch(self.events, key, sender.archive_bytes(files))',
            '        with self._lock:',
        ]),
        NL.join([
            '        watch = SendWatch(self.events, key, sender.archive_bytes(files))',
            '        watch.peer_key = peer_key',
            '        with self._lock:',
        ]),
    ),
    (
        NL.join([
            "        watch.state('asking')",
            '        try:',
            '            outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                     self.settings.name, files, watch)',
        ]),
        NL.join([
            "        watch.state('asking')",
            '        try:',
            '            if peer is None:',
            '                peer = self.looking.find(watch.peer_key, FOLLOW_SECONDS)',
            '                if peer is None:',
            "                    watch.state('failed', reason='gone')",
            '                    return',
            '            try:',
            '                outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                         self.settings.name, files, watch)',
            '            except (ConnectionRefusedError, TimeoutError):',
            '                # Refused or silent before answering: the receiver may be',
            '                # announcing itself again on another port.',
            '                if watch.reached or watch.cancelled:',
            '                    raise',
            '                fresh = self.looking.find(watch.peer_key, FOLLOW_SECONDS)',
            '                if fresh is None or (fresh.address, fresh.port) == (peer.address, peer.port):',
            '                    raise',
            "                logging.info('receiver came back on port %d; sending again', fresh.port)",
            '                peer = fresh',
            '                outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                         self.settings.name, files, watch)',
        ]),
    ),
    (
        'BIND_RETRY_SECONDS = 5' + NL,
        'BIND_RETRY_SECONDS = 5' + NL +
        'FOLLOW_SECONDS = 15  # nekodrop: wait for a receiver that announces itself again' + NL,
    ),
]
engine = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine.read_text(encoding='utf-8')
changed = False
for old, new in FOLLOW:
    if new in text:
        continue
    if text.count(old) != 1:
        sys.exit('engine.py: a line the receiver-follow patch needs moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True
if changed:
    engine.write_text(text, encoding='utf-8')
    print('engine: follows receivers that come back')

# transfer.py: throughput improvements for all cards (Intel, Realtek, ...):
# - 64 KiB DVZip blocks (4x fewer block headers / TLS records, matches AirDrop spec).
# - Adaptive incompressible-stream bypass: after 4 consecutive blocks do not shrink
#   under zlib level 1 (photos, HEIC, MP4, MOV, ZIP), emit DVZIP_STORED directly for
#   the next 32 blocks (2 MiB) before re-probing one block, cutting sender CPU ~90%
#   on media while still compressing text/mixed archives.
# - 0.5 s end-marker wait (down from 2 s) for iPhone videos/Live Photos that omit the
#   final chunked 0-length terminator.
transfer_path = pathlib.Path(sys.argv[1]) / 'transfer.py'
text = transfer_path.read_text(encoding='utf-8')
TRANSFER_PATCHES = [
    ('END_MARKER_WAIT_SECONDS = 2' + NL, 'END_MARKER_WAIT_SECONDS = 0.5  # nekodrop: faster video/Live Photo completion' + NL),
    ('DVZIP_BLOCK_BYTES = 16 * KIB' + NL, 'DVZIP_BLOCK_BYTES = 64 * KIB  # nekodrop: 64 KiB DVZip blocks' + NL),
    (
        NL.join([
            'def encode_dvzip(pieces):',
            '    """Yield the stream in `pieces` as DVZip blocks, each compressed only if',
            '    that makes it smaller."""',
            '    held = bytearray()',
            '    for piece in pieces:',
            '        held += piece',
            '        while len(held) >= DVZIP_BLOCK_BYTES:',
            '            yield _dvzip_block(bytes(held[:DVZIP_BLOCK_BYTES]))',
            '            del held[:DVZIP_BLOCK_BYTES]',
            '    if held:',
            '        yield _dvzip_block(bytes(held))',
        ]),
        NL.join([
            'def _dvzip_stored(data):',
            '    return (len(data) | DVZIP_STORED).to_bytes(4, \'big\') + data',
            '',
            '',
            'def encode_dvzip(pieces):',
            '    """Yield the stream in `pieces` as DVZip blocks, each compressed only if',
            '    that makes it smaller. Skips zlib on sustained incompressible runs (e.g.',
            '    photos/videos) and re-probes periodically so mixed archives still shrink."""',
            '    held = bytearray()',
            '    pos = 0',
            '    incomp_streak = 0',
            '    skip_left = 0',
            '    for piece in pieces:',
            '        held += piece',
            '        while len(held) - pos >= DVZIP_BLOCK_BYTES:',
            '            block = bytes(memoryview(held)[pos:pos + DVZIP_BLOCK_BYTES])',
            '            pos += DVZIP_BLOCK_BYTES',
            '            if skip_left > 0:',
            '                skip_left -= 1',
            '                yield _dvzip_stored(block)',
            '            else:',
            '                out = _dvzip_block(block)',
            '                if int.from_bytes(out[:4], \'big\') & DVZIP_STORED:',
            '                    incomp_streak += 1',
            '                    if incomp_streak >= 4:',
            '                        skip_left = 32',
            '                else:',
            '                    incomp_streak = 0',
            '                yield out',
            '        if pos >= 4 * DVZIP_BLOCK_BYTES:',
            '            del held[:pos]',
            '            pos = 0',
            '    if pos < len(held):',
            '        tail = bytes(memoryview(held)[pos:])',
            '        yield _dvzip_stored(tail) if skip_left > 0 else _dvzip_block(tail)',
        ]),
    ),
]
changed = False
for old, new in TRANSFER_PATCHES:
    if new in text:
        continue
    if text.count(old) != 1:
        sys.exit(f'transfer.py: expected line moved ({old[:48]!r}); update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True
if changed:
    transfer_path.write_text(text, encoding='utf-8')
    print('transfer: 64 KiB DVZip + adaptive stored bypass + 0.5 s tail wait')

# sender.py & receiver.py: 2 MiB socket buffers and TCP_NODELAY so bursty AWDL
# presence slots do not stall behind Winsock's 64 KiB default TCP window.
sender_path = pathlib.Path(sys.argv[1]) / 'sender.py'
text = sender_path.read_text(encoding='utf-8')
old_s = NL.join([
    '        try:',
    '            sock.settimeout(self.timeout)',
    '            sock.connect((self.host, self.port, 0, self.scope))',
])
new_s = NL.join([
    '        try:',
    '            sock.settimeout(self.timeout)',
    '            for opt in ((socket.IPPROTO_TCP, socket.TCP_NODELAY, 1),',
    '                        (socket.SOL_SOCKET, socket.SO_SNDBUF, 2 * 1024 * 1024),',
    '                        (socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)):',
    '                try:',
    '                    sock.setsockopt(*opt)',
    '                except OSError:',
    '                    pass',
    '            sock.connect((self.host, self.port, 0, self.scope))',
])
if new_s not in text:
    if text.count(old_s) != 1:
        sys.exit('sender.py: Connection.connect moved; update patch-engine.py')
    text = text.replace(old_s, new_s, 1)
    sender_path.write_text(text, encoding='utf-8')
    print('sender: 2 MiB socket buffers + TCP_NODELAY')

receiver_path = pathlib.Path(sys.argv[1]) / 'receiver.py'
text = receiver_path.read_text(encoding='utf-8')
old_r = NL.join([
    '    def setup(self):',
    '        self.request.settimeout(transfer.READ_IDLE_TIMEOUT_SECONDS)',
])
new_r = NL.join([
    '    def setup(self):',
    '        self.request.settimeout(transfer.READ_IDLE_TIMEOUT_SECONDS)',
    '        for opt in ((socket.IPPROTO_TCP, socket.TCP_NODELAY, 1),',
    '                    (socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024),',
    '                    (socket.SOL_SOCKET, socket.SO_SNDBUF, 2 * 1024 * 1024)):',
    '            try:',
    '                self.request.setsockopt(*opt)',
    '            except OSError:',
    '                pass',
])
if new_r not in text:
    if text.count(old_r) != 1:
        sys.exit('receiver.py: Handler.setup moved; update patch-engine.py')
    text = text.replace(old_r, new_r, 1)
    receiver_path.write_text(text, encoding='utf-8')
    print('receiver: 2 MiB socket buffers + TCP_NODELAY')

# mdns.py: answer the A record macOS keeps asking for.  macOS 15.8.1 asks
# `A <host>.local` for the receiver's host name and ignores the NSEC record
# that says there is no A record, so the name never resolves and the PC never
# appears in its AirDrop list.  Publish awdl0's IPv4 link-local address next
# to the AAAA record.
mdns = pathlib.Path(sys.argv[1]) / 'mdns.py'
text = mdns.read_text(encoding='utf-8')
changed = False

old = 'TYPE_PTR, TYPE_TXT, TYPE_AAAA, TYPE_SRV, TYPE_NSEC = 12, 16, 28, 33, 47'
new = 'TYPE_A = 1' + NL + old
if 'TYPE_A = 1' not in text:
    if old not in text:
        sys.exit('mdns.py: the type constants moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True

old = 'def __init__(self, service_type, instance, host, address, port, text):'
new = 'def __init__(self, service_type, instance, host, address, port, text, address_v4=None):'
if new not in text:
    if old not in text:
        sys.exit('mdns.py: the Service signature moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True

old = '        self.text = text' + NL
new = old + '        self.address_v4 = address_v4' + NL
if 'self.address_v4 = address_v4' not in text:
    if old not in text:
        sys.exit('mdns.py: the Service body moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True

old = '            _record(self.host_name, TYPE_AAAA, unique, host_ttl, self.address.packed),'
new = NL.join([
    old,
    '            # macOS keeps asking for an A record and ignores the NSEC that says',
    "            # there is none, so answer it with awdl0's IPv4 link-local address.",
    '            _record(self.host_name, TYPE_A, unique, host_ttl,',
    '                    socket.inet_aton(self.address_v4))',
    '            if self.address_v4 else None,',
])
if 'inet_aton(self.address_v4)' not in text:
    if old not in text:
        sys.exit('mdns.py: the AAAA record line moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True

if changed:
    mdns.write_text(text, encoding='utf-8')
print('mdns patched')

# engine.py: give the Service above the address the A record needs.  awdl0
# carries an IPv4 link-local address next to its IPv6 one; reading it here
# keeps mdns.py free of Windows calls.
V4 = NL.join([
    'def _address4():',
    '    """awak0\'s IPv4 link-local address, for the A record macOS asks for."""',
    '    import subprocess',
    '    try:',
    '        out = subprocess.run(["netsh", "interface", "ipv4", "show", "addresses", "awdl0"],',
    '                             capture_output=True, text=True, timeout=5).stdout',
    '        for line in out.splitlines():',
    '            low = line.lower()',
    '            if "ip address" in low or "ip 鍦板潃" in line:',
    '                return line.split(":")[-1].strip()',
    '    except Exception:',
    '        return None',
    '    return None',
    '',
    '',
])
text = engine.read_text(encoding='utf-8')
old = "address, self.settings.port, f'flags={self.settings.flags}'),"
new = "address, self.settings.port, f'flags={self.settings.flags}', _address4()),"
if new not in text:
    if old not in text:
        sys.exit('engine.py: the Service call moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    text = text.replace(NL + 'class ', NL + V4 + 'class ', 1)
    engine.write_text(text, encoding='utf-8')
    print('engine patched (A record address)')


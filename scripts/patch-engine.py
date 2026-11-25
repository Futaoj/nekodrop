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

# #45: the --outdir default called valid_dir('~/Downloads') while the parser was
# built, so with no Downloads folder in the profile (deleted after choosing
# another receive folder) the engine died at start every few seconds, even
# though the client always passes --outdir.  A string default goes through
# `type` only when --outdir is missing, so standalone runs behave as before.
old = "default=receiver.valid_dir('~/Downloads'))"
new = "default='~/Downloads')  # nekodrop: checked only when --outdir is missing (#45)"
if new not in text:
    if text.count(old) != 1:
        sys.exit('engine.py: the --outdir default moved; update patch-engine.py')
    text = text.replace(old, new, 1)
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


# Contacts Only: nekodrop_identity.py (copied beside the engine by
# build-installer.ps1) gives the receiver and the sender an Apple ID identity
# when the client set NEKODROP_IDENTITY: the Apple-signed certificate on TLS,
# the validation record in /Discover and /Ask, and a Contacts Only filter on
# the receiver (NEKODROP_RECEIVE=contacts). Without one nothing changes.
# The hooks below import it, so the patch brings it along: engine-test ran the
# patched engine without it and every test module of receiver/sender/engine
# failed to import (ModuleNotFoundError) from 2.1.0 on.
_identity = pathlib.Path(__file__).resolve().parent.parent / 'installer' / 'nekodrop_identity.py'
(pathlib.Path(sys.argv[1]) / 'nekodrop_identity.py').write_bytes(_identity.read_bytes())


def _hook(name, pairs, mark):
    path = pathlib.Path(sys.argv[1]) / name
    text = path.read_text(encoding='utf-8')
    if mark in text:
        return
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'{name}: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8')
    print(f'{name}: Apple ID identity (Contacts Only)')


_hook('receiver.py', [
    (NL + 'import mdns' + NL, NL + 'import mdns' + NL + 'import nekodrop_identity' + NL),
    (NL.join([
        "        if self.path == '/Discover':",
        '            self.answer(self.identity_plist())',
        '        else:',
        '            self.handle_ask(body)',
    ]), NL.join([
        '        try:',
        "            sender_record = plistlib.loads(body).get('SenderRecordData') if body else None",
        '        except Exception:',
        '            sender_record = None',
        '        if not nekodrop_identity.allowed(sender_record):',
        "            self.reject(401, 'not one of the contacts (Contacts Only)')",
        '            return',
        "        if self.path == '/Discover':",
        '            self.answer(self.identity_plist())',
        '        else:',
        '            self.handle_ask(body)',
    ])),
    ("            'ReceiverModelName': settings.model," + NL,
     "            'ReceiverModelName': settings.model," + NL + '            **nekodrop_identity.receiver_extra(),' + NL),
    (NL.join([
        '    context.load_cert_chain(certificate, key)',
        '    # Senders present self-signed certificates, so none is asked for.',
    ]), NL.join([
        '    if not nekodrop_identity.load_into(context):',
        '        context.load_cert_chain(certificate, key)',
        '    # Senders present self-signed certificates, so none is asked for.',
    ])),
], 'import nekodrop_identity')

_hook('sender.py', [
    (NL + 'import mdns' + NL, NL + 'import mdns' + NL + 'import nekodrop_identity' + NL),
    (NL.join([
        '    context.verify_mode = ssl.CERT_NONE',
        '    return context',
    ]), NL.join([
        '    context.verify_mode = ssl.CERT_NONE',
        '    # With an Apple ID identity the Apple-signed certificate goes on: an',
        '    # iPhone takes that one (a self-signed one it refuses, see above).',
        '    nekodrop_identity.load_into(context)',
        '    return context',
    ])),
    ("post_plist(connection, '/Discover', {})", "post_plist(connection, '/Discover', nekodrop_identity.sender_body({}))"),
    ("        'SenderModelName': DEFAULT_MODEL," + NL,
     "        'SenderModelName': DEFAULT_MODEL," + NL + '        **nekodrop_identity.sender_extra(),' + NL),
], 'import nekodrop_identity')

# macOS 15 can follow the advertised SRV target with an IPv4 A query (#51,
# PR #55). awdl0 transports IPv6 only, so answer for the name we own with an
# explicit negative record, not a Windows APIPA address the peer cannot use.
# RFC 6762 §6.1: synthesized mDNS NSEC must not set the NSEC bit itself.
mdns = pathlib.Path(sys.argv[1]) / 'mdns.py'
text = mdns.read_text(encoding='utf-8')
if 'def address_response(self, types):' not in text:
    pairs = [
        (
            'TYPE_PTR, TYPE_TXT, TYPE_AAAA, TYPE_SRV, TYPE_NSEC = 12, 16, 28, 33, 47',
            'TYPE_A = 1\nTYPE_PTR, TYPE_TXT, TYPE_AAAA, TYPE_SRV, TYPE_NSEC = 12, 16, 28, 33, 47',
        ),
        (
            '# NSEC type bitmap, window 0, with bits 28 (AAAA) and 47 (NSEC) set.\n'
            'HOST_TYPES = bytes([0, 6, 0, 0, 0, 0x08, 0, 0x01])',
            '# Synthesized mDNS NSEC: window 0, only type 28 (AAAA) exists.\n'
            'HOST_TYPES = bytes([0, 4, 0, 0, 0, 0x08])',
        ),
        (
            """def question_names(packet):
    \"\"\"Lower-cased names a query asks about; empty for responses and for
    anything malformed.\"\"\"
    try:
        _, flags, questions = struct.unpack_from('!HHH', packet)
        if flags & FLAG_RESPONSE:
            return set()
        names = set()
        offset = 12
        for _ in range(questions):
            name, offset = decode_name(packet, offset)
            offset += 4
            names.add(name.lower())
        return names
    except (IndexError, struct.error, ValueError):
        return set()""",
            """def questions_in(packet):
    \"\"\"(name, type) from IN-class mDNS questions, including QU questions.\"\"\"
    try:
        _, flags, questions, _, _, _ = struct.unpack_from('!HHHHHH', packet)
        if flags & FLAG_RESPONSE:
            return set()
        found = set()
        offset = HEADER_BYTES
        for _ in range(questions):
            name, offset = decode_name(packet, offset)
            rtype, rclass = struct.unpack_from('!HH', packet, offset)
            offset += 4
            if rclass & ~CACHE_FLUSH == CLASS_IN:
                found.add((name.lower(), rtype))
        return found
    except (IndexError, struct.error, ValueError):
        return set()


def question_names(packet):
    return {name for name, _ in questions_in(packet)}""",
        ),
        (
            '    def response(self, alive=True):',
            """    def address_response(self, types):
        \"\"\"Reply to host address queries, with the requested result in Answer.\"\"\"
        unique = CLASS_IN | CACHE_FLUSH
        aaaa = _record(self.host_name, TYPE_AAAA, unique, TTL_HOST, self.address.packed)
        nsec = _record(self.host_name, TYPE_NSEC, unique, TTL_HOST,
                       encode_name(self.host_name) + HOST_TYPES)
        answers, additional = [], []
        (answers if TYPE_AAAA in types else additional).append(aaaa)
        (answers if TYPE_A in types else additional).append(nsec)
        header = struct.pack('!HHHHHH', 0, RESPONSE_FLAGS, 0, len(answers), 0, len(additional))
        return header + b''.join(answers + additional)

    def response(self, alive=True):""",
        ),
        (
            """        asked = question_names(packet) & self.service.names()
        if asked:
            logging.info('mdns: %s asked for %s', sender[0].split('%')[0],
                         ', '.join(sorted(asked)))
        return bool(asked)""",
            """        names = self.service.names()
        asked = {(name, rtype) for name, rtype in questions_in(packet) if name in names}
        if asked:
            logging.info('mdns: %s asked for %s', sender[0].split('%')[0],
                         ', '.join(f'{name} (type {rtype})' for name, rtype in sorted(asked)))
        return asked""",
        ),
        (
            """        last_sent = 0.0
        while not self.stopping.is_set():
            asked = self._asked()
            since = time.monotonic() - last_sent
            if (asked and since >= MIN_REPLY_GAP_S) or since >= ANNOUNCE_PERIOD_S:
                self._send(announcement)
                last_sent = time.monotonic()""",
            """        last_announce = last_reply = float('-inf')
        while not self.stopping.is_set():
            now = time.monotonic()
            if now - last_announce >= ANNOUNCE_PERIOD_S:
                self._send(announcement)
                last_announce = now
            asked = self._asked()
            now = time.monotonic()
            if not asked or self.stopping.is_set() or now - last_reply < MIN_REPLY_GAP_S:
                continue
            host = self.service.host_name.lower()
            if all(name == host and rtype in (TYPE_A, TYPE_AAAA) for name, rtype in asked):
                self._send(self.service.address_response({rtype for _, rtype in asked}))
            else:
                self._send(announcement)
                last_announce = now
            last_reply = now""",
        ),
    ]
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'mdns.py: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    mdns.write_text(text, encoding='utf-8')
    print('mdns.py: IPv6-only host answers for macOS address queries')

# Link sharing & media format support:
# - Extract http(s) URLs from AirDrop Ask Items even when an app (e.g. Bilibili,
#   Xiaohongshu, WeChat) wraps the link with title text or omits TransferType.links
#   when Files is empty, while keeping is_web_link() strict for safety.
# - Allow opting into original HEIC/HEVC media via NEKODROP_MEDIA_MODE=original
#   (default stays compatible JPEG/H.264).
transfer_path = pathlib.Path(sys.argv[1]) / 'transfer.py'
text = transfer_path.read_text(encoding='utf-8')
if 'def extract_web_links(ask):' not in text:
    old = NL.join([
        'def is_web_link(item):',
        "    return (isinstance(item, str) and item.startswith(('http://', 'https://'))",
        '            and not LINK_BREAKING_CHAR.search(item))',
    ])
    new = NL.join([
        'def is_web_link(item):',
        "    return (isinstance(item, str) and item.startswith(('http://', 'https://'))",
        '            and not LINK_BREAKING_CHAR.search(item))',
        '',
        '',
        "WEB_LINK_RE = re.compile(r'https?://[^\\s<>\"\\'\\x00-\\x1f\\x7f]+')",
        "LINK_TRAILING_PUNCT = '.,;:!?)]}）】》。，；：！？'",
        '',
        '',
        'def extract_web_links(ask):',
        '    """Return safe http(s) URLs carried in an Ask\'s Items (plain URLs, share',
        '    strings such as Bilibili shares containing a URL, or dict values)."""',
        "    items = ask.get('Items') or []",
        "    kinds = ask.get('TransferType') or {}",
        "    files = ask.get('Files') or []",
        "    if 'links' not in kinds and (files or not items):",
        '        return []',
        '    found = []',
        '    for item in items:',
        '        if is_web_link(item):',
        '            if item not in found:',
        '                found.append(item)',
        '            continue',
        '        candidates = []',
        '        if isinstance(item, str):',
        '            candidates = WEB_LINK_RE.findall(item)',
        '        elif isinstance(item, dict):',
        '            for val in item.values():',
        '                if isinstance(val, str):',
        '                    candidates.extend(WEB_LINK_RE.findall(val))',
        '        for raw in candidates:',
        '            cleaned = raw.rstrip(LINK_TRAILING_PUNCT)',
        '            if is_web_link(cleaned) and cleaned not in found:',
        '                found.append(cleaned)',
        '    return found',
    ])
    if text.count(old) != 1:
        sys.exit('transfer.py: is_web_link moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    transfer_path.write_text(text, encoding='utf-8')
    print('transfer.py: extract_web_links for app share strings')

receiver_path = pathlib.Path(sys.argv[1]) / 'receiver.py'
text = receiver_path.read_text(encoding='utf-8')
if 'def _media_capabilities():' not in text:
    pairs = [
        (
            "MEDIA_CAPABILITIES = json.dumps({'Version': 1}).encode()",
            NL.join([
                "MEDIA_CAPABILITIES = json.dumps({'Version': 1}).encode()",
                'MEDIA_CAPABILITIES_ORIGINAL = json.dumps({',
                "    'Version': 1,",
                "    'Codecs': {'hvc1': {'Profiles': {'HEVCMain': {}, 'HEVCMain10': {}}}},",
                "    'ContainerFormats': {'public.heic': {}, 'public.heif': {}, 'public.avci': {}},",
                '}).encode()',
                '',
                '',
                'def _media_capabilities():',
                "    if os.environ.get('NEKODROP_MEDIA_MODE') == 'original':",
                '        return MEDIA_CAPABILITIES_ORIGINAL',
                '    return MEDIA_CAPABILITIES',
            ]),
        ),
        (
            "            'ReceiverMediaCapabilities': MEDIA_CAPABILITIES,",
            "            'ReceiverMediaCapabilities': _media_capabilities(),",
        ),
        (
            NL.join([
                "        if 'links' in kinds:",
                '            links = []',
                '            for item in items:',
                '                if not transfer.is_web_link(item):',
                "                    logging.warning('ask: ignoring non-web item %.80r', item)",
                '                    continue',
                '                stored = transfer.store_link(item, self.server.settings.dest)',
                "                logging.info('link stored as %s', stored)",
                '                links.append(stored)',
                '            watcher.stored(self.accepted, links)',
            ]),
            NL.join([
                '        extracted = transfer.extract_web_links(ask)',
                "        if 'links' in kinds or extracted:",
                '            links = []',
                '            for url in extracted:',
                '                stored = transfer.store_link(url, self.server.settings.dest)',
                "                logging.info('link stored as %s', stored)",
                '                links.append(stored)',
                '            if not extracted and items:',
                '                for item in items:',
                "                    logging.warning('ask: ignoring non-web item %.80r', item)",
                '            watcher.stored(self.accepted, links)',
            ]),
        ),
    ]
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'receiver.py: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    receiver_path.write_text(text, encoding='utf-8')
    print('receiver.py: app link extraction + configurable media capabilities')

engine_path = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine_path.read_text(encoding='utf-8')
if 'urls=urls' not in text:
    pairs = [
        (
            NL.join([
                '    def _open(self, sender_name, model, files, links, waits):',
                '        with self._lock:',
                '            key = next(self._keys)',
                '            if waits:',
                '                # Before the request is said: its answer may come back at once.',
                "                self._waiting[key] = {'made': threading.Event(), 'accept': False}",
                "        sizes = [file['size'] for file in files]",
                '        total = sum(sizes) if sizes and None not in sizes else None',
                "        self.events.say('asked', id=key, sender=sender_name, model=model, files=files,",
                '                        links=links, total=total, waits=waits)',
                '        return Accepted(key, total)',
                '',
                '    def asked(self, ask, address, left):',
                "        kinds = ask.get('TransferType') or {}",
                "        links = sum(1 for item in ask.get('Items') or [] if transfer.is_web_link(item)) \\",
                "            if 'links' in kinds else 0",
                '        waits = self.settings.ask',
                "        accepted = self._open(ask.get('SenderComputerName'), ask.get('SenderModelName'),",
                '                              announced_files(ask), links, waits)',
            ]),
            NL.join([
                '    def _open(self, sender_name, model, files, links, waits, urls=None):',
                '        with self._lock:',
                '            key = next(self._keys)',
                '            if waits:',
                '                # Before the request is said: its answer may come back at once.',
                "                self._waiting[key] = {'made': threading.Event(), 'accept': False}",
                "        sizes = [file['size'] for file in files]",
                '        total = sum(sizes) if sizes and None not in sizes else None',
                "        self.events.say('asked', id=key, sender=sender_name, model=model, files=files,",
                '                        links=links, urls=urls or [], total=total, waits=waits)',
                '        return Accepted(key, total)',
                '',
                '    def asked(self, ask, address, left):',
                '        urls = transfer.extract_web_links(ask)',
                '        links = len(urls)',
                '        waits = self.settings.ask',
                "        accepted = self._open(ask.get('SenderComputerName'), ask.get('SenderModelName'),",
                '                              announced_files(ask), links, waits, urls=urls)',
            ]),
        ),
    ]
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'engine.py: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    engine_path.write_text(text, encoding='utf-8')
    print('engine.py: include extracted link URLs in asked events')

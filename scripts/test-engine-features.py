# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for nekodrop's patched AirDrop engine."""
import gzip
import io
import json
import os
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

if len(sys.argv) < 2:
    sys.exit('usage: test-engine-features.py <patched airdrop dir> [unittest args]')

sys.path.insert(0, sys.argv[1])
del sys.argv[1]

import receiver  # noqa: E402
import sender  # noqa: E402
import transfer  # noqa: E402


class LinkExtractionTests(unittest.TestCase):
    def test_plain_web_link(self):
        ask = {'TransferType': {'links': True}, 'Items': ['https://example.org/page']}
        self.assertEqual(transfer.extract_web_links(ask), ['https://example.org/page'])

    def test_bilibili_share_text_extracts_url(self):
        ask = {
            'TransferType': {'links': True},
            'Items': ['【实测 Windows 原生隔空投送】 https://b23.tv/abc1234）'],
        }
        self.assertEqual(transfer.extract_web_links(ask), ['https://b23.tv/abc1234'])

    def test_items_without_transfer_type_when_no_files(self):
        ask = {'Items': ['https://www.bilibili.com/video/BV1xx411c7mD']}
        self.assertEqual(
            transfer.extract_web_links(ask),
            ['https://www.bilibili.com/video/BV1xx411c7mD'],
        )

    def test_dict_item_with_url(self):
        ask = {'Items': [{'Title': 'Video', 'URL': 'https://b23.tv/xyz999'}]}
        self.assertEqual(transfer.extract_web_links(ask), ['https://b23.tv/xyz999'])

    def test_rejects_file_and_javascript_schemes(self):
        ask = {
            'TransferType': {'links': True},
            'Items': ['file:///C:/Windows/win.ini', 'javascript:alert(1)'],
        }
        self.assertEqual(transfer.extract_web_links(ask), [])

    def test_extracts_url_from_mixed_files_and_items(self):
        ask = {
            'TransferType': {'files': True},
            'Files': [{'FileName': 'cover.jpeg'}],
            'Items': ['【视频标题】 https://b23.tv/mixed123'],
        }
        self.assertEqual(transfer.extract_web_links(ask), ['https://b23.tv/mixed123'])

    def test_save_preview_icon_jpeg_and_rejects_junk(self):
        fake_jpg = b'\xff\xd8\xff\xe0' + b'\x00' * 32
        path = transfer.save_preview_icon({'FileIcon': fake_jpg}, 't1')
        self.assertTrue(path.endswith('.jpg'))
        self.assertTrue(os.path.isfile(path))
        os.unlink(path)
        self.assertEqual(transfer.save_preview_icon({'FileIcon': b'not an image at all!'}, 't2'), '')


class UploadCompletionTests(unittest.TestCase):
    archive = b''.join(transfer.cpio.write_archive([('./sample.txt', 0o100644, 5, 0, [b'hello'])]))

    def test_complete_codec_and_cpio_across_split_writes(self):
        compressed = zlib.compress(self.archive)
        bodies = [gzip.compress(self.archive),
                  len(compressed).to_bytes(4, 'big') + compressed,
                  (0x80000000 | len(self.archive)).to_bytes(4, 'big') + self.archive]
        for body in bodies:
            with self.subTest(prefix=body[:4]):
                watched = transfer.EndWatchingFile(io.BytesIO(), lambda: None)
                for byte in body[:-1]:
                    watched.write(bytes([byte]))
                    self.assertFalse(watched.at_stream_end)
                watched.write(body[-1:])
                self.assertTrue(watched.at_stream_end)
                watched.write(b'\x80')
                self.assertFalse(watched.at_stream_end)

    def receive(self, payload, tail, dest):
        # End-of-input models a silent, still-connected sender. Record which
        # timeout is active at the exact read that stalled.
        deadline = transfer.ReadDeadline()
        request = mock.Mock()
        request.timeout = deadline.idle_seconds
        request.settimeout.side_effect = lambda value: setattr(request, 'timeout', value)

        class Input(io.BytesIO):
            def read(self, size):
                data = super().read(size)
                if not data:
                    self.stalled_timeout = request.timeout
                    raise TimeoutError(request.timeout)
                return data

            def readline(self, size):
                data = super().readline(size)
                if not data.endswith(b'\n'):
                    raise TimeoutError(request.timeout)
                return data

        body = b'%x\r\n' % len(payload) + payload + b'\r\n' + tail
        handler = SimpleNamespace(request=request, rfile=Input(body), headers={},
                                  server=SimpleNamespace(settings=SimpleNamespace(dest=dest)))
        with tempfile.TemporaryFile() as raw:
            result = receiver.Handler.receive_archive(handler, raw, transfer.ReceiveBudget(dest, 30),
                                                      ('chunked', None), deadline, 'test', None)
        if not result[1]:
            self.assertEqual(handler.rfile.stalled_timeout, transfer.COMPLETE_CHUNK_WAIT_SECONDS)
        self.assertEqual(request.timeout, deadline.idle_seconds)
        return result

    def test_missing_and_explicit_terminators_store_identical_files(self):
        for tail in (b'', b'0\r\n\r\n'):
            with self.subTest(tail=tail), tempfile.TemporaryDirectory() as dest:
                names, ended = self.receive(gzip.compress(self.archive), tail, dest)
                self.assertEqual(names, ['sample.txt'])
                self.assertEqual(ended, bool(tail))
                self.assertEqual((Path(dest) / names[0]).read_bytes(), b'hello')

    def test_silent_partial_header_and_invalid_archives_do_not_publish(self):
        full = gzip.compress(self.archive)
        stored = (0x80000000 | len(self.archive)).to_bytes(4, 'big') + self.archive
        cut_block = zlib.compress(self.archive)[:-1]
        incomplete_then_stored = len(cut_block).to_bytes(4, 'big') + cut_block + stored
        for payload, tail in ((full, b'0'), (incomplete_then_stored, b''), (full[:-4], b''), (full + b'x', b''),
                              (stored + b'\x80', b''), (gzip.compress(self.archive[:32]), b'')):
            with self.subTest(length=len(payload), tail=tail), tempfile.TemporaryDirectory() as dest:
                with self.assertRaises(TimeoutError) as stall:
                    self.receive(payload, tail, dest)
                self.assertEqual(stall.exception.args, (transfer.READ_IDLE_TIMEOUT_SECONDS,))
                self.assertEqual(list(Path(dest).iterdir()), [])


class MediaCapabilitiesTests(unittest.TestCase):
    def tearDown(self):
        os.environ.pop('NEKODROP_MEDIA_MODE', None)

    def test_default_is_compatible_v1(self):
        os.environ.pop('NEKODROP_MEDIA_MODE', None)
        caps = json.loads(receiver._media_capabilities().decode())
        self.assertEqual(caps, {'Version': 1})

    def test_original_mode_advertises_heic_and_hevc(self):
        os.environ['NEKODROP_MEDIA_MODE'] = 'original'
        caps = json.loads(receiver._media_capabilities().decode())
        self.assertEqual(caps['Version'], 1)
        self.assertIn('hvc1', caps['Codecs'])
        self.assertIn('public.heic', caps['ContainerFormats'])


class BeaconCoordinationTests(unittest.TestCase):
    def setUp(self):
        sender.Beacon._owner = None
        sender.Beacon._refs = 0
        sender.Beacon._looking_refs = 0

    def tearDown(self):
        sender.Beacon._owner = None
        sender.Beacon._refs = 0
        sender.Beacon._looking_refs = 0

    def test_single_publisher_with_rising_edge_when_looking_joins_standby(self):
        started = []

        class FakeProc:
            def __init__(self, env):
                self.env = env
                self.stdout = mock.Mock()
                self.stdout.readline.return_value = sender.BEACON_READY + '\n'
                self.terminated = False

            def poll(self):
                return 0 if self.terminated else None

            def terminate(self):
                self.terminated = True

            def wait(self):
                return 0

        def fake_popen(cmd, **kwargs):
            proc = FakeProc(kwargs.get('env', {}))
            started.append(proc)
            return proc

        def read_text(path):
            with open(path, encoding='ascii') as handle:
                return handle.read().strip()

        with mock.patch.object(sender.subprocess, 'Popen', side_effect=fake_popen):
            with sender.Beacon(standby_pulse=60, role='standby') as standby:
                self.assertEqual(len(started), 1)
                mode_file = started[0].env['NEKODROP_BEACON_MODE']
                pulse_file = started[0].env['NEKODROP_BEACON_PULSE']
                self.assertEqual(read_text(mode_file), 'standby')
                self.assertFalse(os.path.exists(pulse_file))
                with sender.Beacon(role='looking') as looking:
                    # Must NOT spawn a second WinRT BLE publisher, MUST switch mode to
                    # looking, and MUST trigger a fresh 2.5 s rising-edge repulse.
                    self.assertEqual(len(started), 1)
                    self.assertTrue(looking.on)
                    self.assertEqual(read_text(mode_file), 'looking')
                    self.assertTrue(os.path.exists(pulse_file))
                # Exiting looking restores standby mode without killing standby publisher.
                self.assertFalse(started[0].terminated)
                self.assertEqual(read_text(mode_file), 'standby')
            self.assertTrue(started[0].terminated)

    def test_idle_peer_gone_does_not_repulse_ble(self):
        engine_path = os.path.join(os.path.dirname(sender.__file__), 'engine.py')
        with open(engine_path, encoding='utf-8') as handle:
            src = handle.read()
        self.assertNotIn("repulse_beacon('peer_gone')", src)
        self.assertNotIn('standby_pulse=60', src)
        self.assertIn("repulse_beacon('post_send')", src)
        self.assertIn("repulse_beacon('send_refused')", src)


if __name__ == '__main__':
    unittest.main()

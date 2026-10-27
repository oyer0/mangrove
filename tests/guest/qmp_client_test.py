#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regressions for bounded QMP response and screenshot handling."""

import json
import os
import tempfile
import threading
import time
from pathlib import Path
from types import SimpleNamespace

from qmp_client import QMP, QMPError, QMPTimeout


class PipePeer:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer

    def recv(self, length):
        return os.read(self.reader, length)

    def sendall(self, payload):
        offset = 0
        while offset < len(payload):
            offset += os.write(self.writer, payload[offset:])

    def settimeout(self, _seconds):
        return None

    def close(self):
        for descriptor in (self.reader, self.writer):
            try:
                os.close(descriptor)
            except OSError:
                pass


def qmp_pipe_pair():
    server_to_client_read, server_to_client_write = os.pipe()
    client_to_server_read, client_to_server_write = os.pipe()
    reader = os.fdopen(server_to_client_read, 'rb', buffering=0)
    writer = os.fdopen(client_to_server_write, 'wb', buffering=0)
    return (SimpleNamespace(stdout=reader, stdin=writer),
            PipePeer(client_to_server_read, server_to_client_write))


def read_message(peer):
    line = bytearray()
    while not line.endswith(b'\n') and len(line) <= 65536:
        chunk = peer.recv(1)
        if not chunk:
            break
        line.extend(chunk)
    if not line or len(line) > 65536 or not line.endswith(b'\n'):
        raise AssertionError('client sent no bounded QMP command')
    return json.loads(line)


def send_message(peer, message):
    peer.sendall(json.dumps(message, separators=(',', ':')).encode() + b'\n')


def test_greeting_events_ids_and_fresh_screendump():
    process, peer = qmp_pipe_pair()
    failures = []

    def server():
        try:
            greeting = json.dumps({'QMP': {'version': {'qemu': {'major': 9}},
                                           'capabilities': []}}).encode() + b'\n'
            peer.sendall(greeting[:4])
            peer.sendall(greeting[4:])
            command = read_message(peer)
            assert command == {'execute': 'qmp_capabilities', 'id': 1}
            peer.sendall((json.dumps({'event': 'RESET', 'data': {}}) + '\n' +
                          json.dumps({'return': {}, 'id': 1}) + '\n').encode())

            command = read_message(peer)
            assert command == {'execute': 'query-status', 'id': 2}
            send_message(peer, {'return': {'status': 'running', 'running': True},
                                'id': 2})

            command = read_message(peer)
            assert command['execute'] == 'screendump' and command['id'] == 3
            target = Path(command['arguments']['filename'])
            target.write_bytes(b'P6\n1 1\n255\n\x01\x02\x03')
            send_message(peer, {'return': {}, 'id': 3})

            command = read_message(peer)
            assert command == {'execute': 'test-error', 'id': 4}
            send_message(peer, {'error': {'class': 'GenericError',
                                          'desc': 'controlled failure'},
                                'id': 4})
        except Exception as error:
            failures.append(error)
        finally:
            peer.close()

    thread = threading.Thread(target=server)
    thread.start()
    qmp = QMP(process, timeout=1)
    assert qmp.greeting['version']['qemu']['major'] == 9
    assert qmp.events == [{'event': 'RESET', 'data': {}}]
    assert qmp.query_status() == 'running'
    with tempfile.TemporaryDirectory(prefix='mangrove-qmp-') as directory:
        image = Path(directory) / 'screen.ppm'
        image.write_bytes(b'stale screenshot')
        assert qmp.screendump(image, status='running') == image
        assert image.read_bytes() == b'P6\n1 1\n255\n\x01\x02\x03'
    try:
        qmp.call('test-error')
    except QMPError as error:
        assert 'test-error' in str(error) and 'controlled failure' in str(error)
    else:
        raise AssertionError('QMP command error was accepted')
    qmp.close()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert not failures, failures


def test_screendump_requires_running_state():
    process, peer = qmp_pipe_pair()
    failures = []

    def server():
        try:
            send_message(peer, {'QMP': {'version': {}, 'capabilities': []}})
            assert read_message(peer)['execute'] == 'qmp_capabilities'
            send_message(peer, {'return': {}, 'id': 1})
            command = read_message(peer)
            assert command == {'execute': 'query-status', 'id': 2}
            send_message(peer, {'return': {'status': 'paused', 'running': False},
                                'id': 2})
            peer.settimeout(1)
            if peer.recv(1):
                raise AssertionError('screendump was sent for a paused guest')
        except Exception as error:
            failures.append(error)
        finally:
            peer.close()

    thread = threading.Thread(target=server)
    thread.start()
    qmp = QMP(process, timeout=1)
    with tempfile.TemporaryDirectory(prefix='mangrove-qmp-state-') as directory:
        image = Path(directory) / 'screen.ppm'
        try:
            qmp.screendump(image)
        except QMPError as error:
            assert 'paused' in str(error)
        else:
            raise AssertionError('paused guest screenshot was accepted')
        assert not image.exists()
    qmp.close()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert not failures, failures


def test_command_deadline_names_pending_request():
    process, peer = qmp_pipe_pair()
    waiting = threading.Event()
    release_server = threading.Event()
    failures = []

    def server():
        try:
            send_message(peer, {'QMP': {'version': {}, 'capabilities': []}})
            command = read_message(peer)
            assert command['id'] == 1
            send_message(peer, {'return': {}, 'id': 1})
            command = read_message(peer)
            assert command == {'execute': 'query-status', 'id': 2}
            waiting.set()
            release_server.wait(1)
        except Exception as error:
            failures.append(error)
        finally:
            peer.close()

    thread = threading.Thread(target=server)
    thread.start()
    qmp = QMP(process, timeout=1)
    try:
        qmp.call('query-status', timeout=0.05)
    except QMPTimeout as error:
        assert "query-status" in str(error) and 'id 2' in str(error)
    except QMPError as error:
        release_server.set()
        qmp.close()
        thread.join(timeout=2)
        raise AssertionError(f'QMP failed before its deadline: {failures!r}') from error
    else:
        raise AssertionError('missing QMP response did not time out')
    finally:
        release_server.set()
        qmp.close()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert not failures, failures


def test_event_stream_cannot_extend_command_deadline():
    process, peer = qmp_pipe_pair()
    stopped = threading.Event()
    failures = []

    def server():
        try:
            send_message(peer, {'QMP': {'version': {}, 'capabilities': []}})
            assert read_message(peer)['id'] == 1
            send_message(peer, {'return': {}, 'id': 1})
            assert read_message(peer)['id'] == 2
            while not stopped.is_set():
                send_message(peer, {'event': 'TICK', 'data': {}})
                time.sleep(0.002)
        except OSError:
            # The client closes its pipe after the bounded command timeout.
            pass
        except Exception as error:
            failures.append(error)
        finally:
            peer.close()

    thread = threading.Thread(target=server)
    thread.start()
    qmp = QMP(process, timeout=1)
    started = time.monotonic()
    try:
        qmp.call('query-status', timeout=0.05)
    except QMPTimeout:
        elapsed = time.monotonic() - started
        assert elapsed < 0.25, f'events extended command deadline: {elapsed:.3f}s'
    else:
        raise AssertionError('event stream kept command alive past its deadline')
    finally:
        stopped.set()
        qmp.close()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert not failures, failures


def test_unexpected_response_id_is_rejected():
    process, peer = qmp_pipe_pair()
    failures = []

    def server():
        try:
            send_message(peer, {'QMP': {'version': {}, 'capabilities': []}})
            assert read_message(peer)['id'] == 1
            send_message(peer, {'return': {}, 'id': 1})
            assert read_message(peer)['id'] == 2
            send_message(peer, {'return': {}, 'id': 99})
        except Exception as error:
            failures.append(error)
        finally:
            peer.close()

    thread = threading.Thread(target=server)
    thread.start()
    qmp = QMP(process, timeout=1)
    try:
        qmp.call('query-status')
    except QMPError as error:
        assert 'unexpected id 99' in str(error)
    else:
        raise AssertionError('mismatched QMP response id was accepted')
    qmp.close()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert not failures, failures


if __name__ == '__main__':
    test_greeting_events_ids_and_fresh_screendump()
    test_screendump_requires_running_state()
    test_command_deadline_names_pending_request()
    test_event_stream_cannot_extend_command_deadline()
    test_unexpected_response_id_is_rejected()
    print('QMP: greeting, event interleaving, command IDs/deadlines, guest state and fresh screenshots passed')

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded QMP client for Mangrove's native guest runners."""

import json
import os
import select
import time
from pathlib import Path


MAX_QMP_LINE = 65536
MAX_QMP_EVENTS = 1024
DEFAULT_QMP_TIMEOUT = 10.0


class QMPError(RuntimeError):
    """QMP protocol, command, or transport failure."""


class QMPTimeout(QMPError, TimeoutError):
    """A QMP operation did not complete within its deadline."""


class QMP:
    def __init__(self, process, reader=None, writer=None,
                 timeout=DEFAULT_QMP_TIMEOUT):
        if timeout <= 0:
            raise ValueError('QMP timeout must be positive')
        self.process = process
        self.reader = reader if reader is not None else process.stdout
        self.writer = writer if writer is not None else process.stdin
        if self.reader is None or self.writer is None:
            raise QMPError('QMP requires readable and writable streams')
        self.timeout = timeout
        self.events = []
        self._receive_buffer = bytearray()
        self._next_id = 1
        self._closed = False
        try:
            greeting = self.receive(
                time.monotonic() + self.timeout, 'QMP greeting')
            if not isinstance(greeting.get('QMP'), dict):
                raise QMPError('QMP greeting did not contain a QMP version')
            self.greeting = greeting['QMP']
            self.call('qmp_capabilities')
        except Exception:
            self.close()
            raise

    @staticmethod
    def _decode_line(line, operation):
        if len(line) > MAX_QMP_LINE:
            raise QMPError(f'{operation}: QMP response exceeded the line limit')
        try:
            message = json.loads(line)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise QMPError(f'{operation}: malformed QMP JSON: {error}') from error
        if not isinstance(message, dict):
            raise QMPError(f'{operation}: QMP response was not an object')
        return message

    def receive(self, deadline=None, operation='QMP response'):
        """Return one bounded JSON message before a monotonic deadline."""
        if deadline is None:
            deadline = time.monotonic() + self.timeout
        descriptor = self.reader.fileno()
        while True:
            newline = self._receive_buffer.find(b'\n')
            if newline >= 0:
                line = bytes(self._receive_buffer[:newline])
                del self._receive_buffer[:newline + 1]
                return self._decode_line(line, operation)
            if len(self._receive_buffer) > MAX_QMP_LINE:
                raise QMPError(f'{operation}: QMP response exceeded the line limit')

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise QMPTimeout(f'{operation}: timed out waiting for QMP')
            try:
                readable, _, _ = select.select([descriptor], [], [], remaining)
            except (OSError, ValueError) as error:
                raise QMPError(f'{operation}: QMP wait failed: {error}') from error
            if not readable:
                raise QMPTimeout(f'{operation}: timed out waiting for QMP')
            try:
                chunk = os.read(descriptor, 4096)
            except BlockingIOError:
                continue
            except OSError as error:
                raise QMPError(f'{operation}: QMP read failed: {error}') from error
            if not chunk:
                raise QMPError(f'{operation}: QMP stream closed')
            self._receive_buffer.extend(chunk)

    def _send(self, payload, deadline, operation):
        descriptor = self.writer.fileno()
        offset = 0
        while offset < len(payload):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise QMPTimeout(f'{operation}: timed out writing QMP request')
            try:
                _, writable, _ = select.select([], [descriptor], [], remaining)
            except (OSError, ValueError) as error:
                raise QMPError(f'{operation}: QMP write wait failed: {error}') from error
            if not writable:
                raise QMPTimeout(f'{operation}: timed out writing QMP request')
            try:
                written = os.write(descriptor, payload[offset:])
            except BlockingIOError:
                continue
            except OSError as error:
                raise QMPError(f'{operation}: QMP write failed: {error}') from error
            if written <= 0:
                raise QMPError(f'{operation}: QMP write made no progress')
            offset += written

    def call(self, name, arguments=None, timeout=None):
        duration = self.timeout if timeout is None else timeout
        if duration <= 0:
            raise ValueError('QMP timeout must be positive')
        request_id = self._next_id
        self._next_id += 1
        request = {'execute': name, 'id': request_id}
        if arguments is not None:
            request['arguments'] = arguments
        payload = json.dumps(request, separators=(',', ':')).encode() + b'\n'
        operation = f'QMP command {name!r} (id {request_id})'
        deadline = time.monotonic() + duration
        self._send(payload, deadline, operation)

        while True:
            reply = self.receive(deadline, operation)
            if 'event' in reply:
                if len(self.events) >= MAX_QMP_EVENTS:
                    raise QMPError('QMP event queue exceeded its bound')
                self.events.append(reply)
                continue
            if reply.get('id') != request_id:
                raise QMPError(
                    f'{operation}: received response for unexpected id '
                    f'{reply.get("id")!r}')
            if 'error' in reply:
                error = reply['error']
                description = (error.get('desc', repr(error))
                               if isinstance(error, dict) else repr(error))
                raise QMPError(f'{operation} failed: {description}')
            if 'return' not in reply:
                raise QMPError(f'{operation}: response has no return or error')
            return reply['return']

    def query_status(self):
        response = self.call('query-status')
        status = response.get('status') if isinstance(response, dict) else None
        if not isinstance(status, str):
            raise QMPError('QMP query-status returned no status')
        if status == 'running' and response.get('running') is False:
            raise QMPError('QMP query-status reported contradictory running state')
        return status

    def screendump(self, path, status=None):
        """Capture a fresh PPM only while the guest is in the running state."""
        path = Path(path)
        if status is None:
            status = self.query_status()
        if status != 'running':
            raise QMPError(f'QMP screendump requires a running guest (state: {status})')
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        self.call('screendump', {'filename': str(path)})
        try:
            with path.open('rb') as image:
                magic = image.read(2)
            if magic != b'P6':
                raise QMPError('QMP screendump did not create a PPM image')
        except OSError as error:
            raise QMPError(f'QMP screendump did not create {path}: {error}') from error
        return path

    def drain_events(self):
        """Collect immediately available final events after the process stops."""
        descriptor = self.reader.fileno()
        drained = bytearray(self._receive_buffer)
        self._receive_buffer.clear()
        while select.select([descriptor], [], [], 0)[0]:
            chunk = os.read(descriptor, min(4096, MAX_QMP_LINE + 1 - len(drained)))
            if not chunk:
                break
            drained.extend(chunk)
            if len(drained) > MAX_QMP_LINE:
                raise QMPError('QMP final event data exceeded the size bound')
        lines = drained.splitlines()
        if drained and not drained.endswith(b'\n'):
            raise QMPError('QMP ended with a truncated final response')
        for line in lines:
            message = self._decode_line(line, 'QMP final event drain')
            if 'event' in message:
                if len(self.events) >= MAX_QMP_EVENTS:
                    raise QMPError('QMP event queue exceeded its bound')
                self.events.append(message)

    def close(self):
        if self._closed:
            return
        self._closed = True
        streams = []
        for stream in (self.writer, self.reader):
            if stream not in streams:
                streams.append(stream)
        for stream in streams:
            try:
                stream.close()
            except OSError:
                pass

    def wait_deleted(self, device, timeout=None):
        duration = self.timeout if timeout is None else timeout
        if duration <= 0:
            raise ValueError('QMP timeout must be positive')
        deadline = time.monotonic() + duration
        while not any(e.get('event') == 'DEVICE_DELETED' and
                      e.get('data', {}).get('device') == device
                      for e in self.events):
            event = self.receive(deadline, f'QMP DEVICE_DELETED {device!r}')
            if 'event' not in event:
                raise QMPError('unexpected QMP command response while waiting for device deletion')
            if len(self.events) >= MAX_QMP_EVENTS:
                raise QMPError('QMP event queue exceeded its bound')
            self.events.append(event)

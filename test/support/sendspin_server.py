#!/usr/bin/env python3
"""The sendspin node's test server (docs/design/sendspin.md section 14): a real SendspinServer, a command per stdin line,
an event per stdout line, so a test moves when an event arrives.

Arguments: --listen 0 [--slow-upgrade <seconds>] (a later slice adds --dial and --state).
Commands: trust (approve the client for unpaired playback), play <seconds> (the ramp), end (stream/end now), quit.
Events: ready <url>, upgrade (a WebSocket upgrade arrived), connected <client id>, play-start <us>, play-finished <frames>,
disconnected <reason>.

The ramp: left is the low 16 bits of the frame index, right the high 16, so every output frame names its place.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys

from aiohttp import web
from aiosendspin.noise.keys import Identity
from aiosendspin.noise.trust_store import InMemoryServerPairingStore
from aiosendspin.server.audio import AudioFormat
from aiosendspin.server.push_stream import StreamStoppedError
from aiosendspin.server.server import (
    ClientConnectedEvent,
    ClientDisconnectedEvent,
    SendspinServer,
)

RATE = 48000
CHUNK_FRAMES = 1200  # 25 ms
MAX_AHEAD_US = 1_000_000
PLAYED_OUT_MARGIN_S = 0.2
SERVER_NAME = "ear-test-server"


def emit(event: str) -> None:
    print(event, flush=True)


def ramp(start: int, frames: int) -> bytes:
    pcm = bytearray()
    for index in range(start, start + frames):
        pcm += (index & 0xFFFF).to_bytes(2, "little") + ((index >> 16) & 0xFFFF).to_bytes(2, "little")
    return bytes(pcm)


class TestServer:
    def __init__(self, loop: asyncio.AbstractEventLoop, slow_upgrade: float) -> None:
        self._slow_upgrade = slow_upgrade
        self._client_id: str | None = None
        self._play: asyncio.Task[None] | None = None
        # Music Assistant's default: a client off the spec is refused.
        self._server = SendspinServer(
            loop,
            Identity.generate(),
            SERVER_NAME,
            pairing_store=InMemoryServerPairingStore(),
            allow_noncompliant_clients=False,
        )
        self._server.add_event_listener(self._on_event)

    def _on_event(self, _server: SendspinServer, event: object) -> None:
        if isinstance(event, ClientConnectedEvent):
            self._client_id = event.client_id
            emit(f"connected {event.client_id}")
        elif isinstance(event, ClientDisconnectedEvent):
            reason = event.goodbye_reason.value if event.goodbye_reason is not None else "none"
            emit(f"disconnected {reason}")

    async def _upgrade(self, request: web.Request) -> web.StreamResponse:
        emit("upgrade")
        if self._slow_upgrade > 0:
            await asyncio.sleep(self._slow_upgrade)
        return await self._server.on_client_connect(request)

    async def listen(self) -> None:
        app = web.Application()
        app.router.add_get(SendspinServer.API_PATH, self._upgrade)
        self._runner = web.AppRunner(app)
        await self._runner.setup()
        site = web.TCPSite(self._runner, "127.0.0.1", 0)
        await site.start()
        port = self._runner.addresses[0][1]
        emit(f"ready ws://127.0.0.1:{port}{SendspinServer.API_PATH}")

    def _group(self):
        client = self._server.get_client(self._client_id) if self._client_id else None
        if client is None:
            raise RuntimeError("no client")
        return client.group

    async def trust(self) -> None:
        await self._server.trust_unpaired(self._client_id)

    async def _player_active(self) -> None:
        """Until the client's player role takes audio: `trust` activates it, and the client answers in its own time.

        The library has no event for this; a stream started before it would join the ramp late, part way in.
        """
        client = self._server.get_client(self._client_id)
        while client is None or client.awaits_role_state("player") or not client.roles_by_family("player"):
            await asyncio.sleep(0.005)
            client = self._server.get_client(self._client_id)

    async def _play_ramp(self, seconds: float) -> None:
        await self._player_active()
        group = self._group()
        stream = group.start_stream()
        audio_format = AudioFormat(RATE, 16, 2)
        total = int(seconds * RATE)
        first_us = None
        for start in range(0, total, CHUNK_FRAMES):
            stream.prepare_audio(ramp(start, min(CHUNK_FRAMES, total - start)), audio_format)
            try:
                start_us = await stream.commit_audio()
            except StreamStoppedError:
                return  # the server closed under it, as a test ends
            if first_us is None:
                first_us = start_us
                emit(f"play-start {first_us}")
            await stream.sleep_to_limit_buffer(MAX_AHEAD_US)
        # stream/end clears what the client holds, so a stream ends once it has played out, as a track does.
        left_us = first_us + total * 1_000_000 // RATE - stream.now_us()
        await asyncio.sleep(max(left_us / 1_000_000, 0.0) + PLAYED_OUT_MARGIN_S)
        emit(f"play-finished {total}")
        group.stop_stream()

    def play(self, seconds: float) -> None:
        self._play = asyncio.ensure_future(self._play_ramp(seconds))
        self._play.add_done_callback(self._played)

    @staticmethod
    def _played(task: asyncio.Task[None]) -> None:
        if not task.cancelled() and task.exception() is not None:
            logging.error("play failed", exc_info=task.exception())

    def end(self) -> None:
        if self._play is not None:
            self._play.cancel()
            self._play = None
        self._group().stop_stream()

    async def close(self) -> None:
        if self._play is not None:
            self._play.cancel()
        await self._server.close()
        await self._runner.cleanup()


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", type=int, required=True)
    parser.add_argument("--slow-upgrade", type=float, default=0.0)
    arguments = parser.parse_args()

    loop = asyncio.get_running_loop()
    server = TestServer(loop, arguments.slow_upgrade)
    await server.listen()

    reader = asyncio.StreamReader()
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
    while line := await reader.readline():
        command, *rest = line.decode().split()
        if command == "trust":
            await server.trust()
        elif command == "play":
            server.play(float(rest[0]))
        elif command == "end":
            server.end()
        elif command == "quit":
            break
    await server.close()


if __name__ == "__main__":
    logging.basicConfig(stream=sys.stderr, level=logging.WARNING)
    asyncio.run(main())

#!/usr/bin/env python3
"""The sendspin node's test server (docs/node-types.md#sendspin): a real SendspinServer, a command per stdin line,
an event per stdout line, so a test moves when an event arrives.

Arguments: --listen 0 [--slow-upgrade <seconds>] [--dial <url>] [--state <dir>] [--discover <prefix>] [--name <text>] [--hold-activate]. It always listens
(a dialling node needs that, and the tests' spawn always asks); --dial also dials a listening node, retrying until it answers.
--state keeps its identity and its pairings in <dir> across restarts. --discover browses mDNS with aiosendspin's own discovery,
which dials what it finds, among only the instances starting with <prefix>, since tests running side by side share a network.
Commands: dial <url>, use <client id> (later commands act on that client; each `connected` moves them to the new one), trust (approve the client for unpaired playback), roles (say which roles it holds), play <seconds> (the ramp),
end (stream/end now), prefer-16 (ask for FLAC 16, as Music Assistant's per-player format setting does), lead (say the player's required_lead_time_ms), pair (a dynamic-code attempt, as Music Assistant starts one), code <digits> (what the operator
typed: one per round), gap <ms> (hold the audio sent until the client has starved for about <ms>, then send it at
once: _Gap), source-start, source-stop, quit. --name is what the server calls itself. --hold-activate stops each connection after the hellos, before its first server/activate, until the command activate-release: a server the client has proved or not by its Noise handshake, and not yet admitted.
Events: ready <url>, upgrade (a WebSocket upgrade arrived), connected <client id>, trusted, roles <list|none>,
play-start <us>, chunk-sent <first frame> <frames> <us> (a chunk of the ramp handed to the client, the first of its frames
stamped to play at <us>), play-finished <frames>, pair-pending (the client held the attempt back), pair-finished,
pair-failed <reason>, source-started <rate> <channels> <bits>, source-chunk <frames> <stamp, us> <its arrival on this
server's clock - its stamp, us> (each chunk of the source's audio as it arrives), source-heard (a second of it has arrived),
source-ended <frames> <left rms> <non-zero right samples>, prefer-16 <true|false: whether the player took it>, lead <ms>,
gap-held <us: the stamp the client's audio ends at>, gap-released <chunks> <stamp, us> <duration, us> <us: when, of
the chunk the release fell in> <us: the first chunk sent>, disconnected <reason>, activate-held, activate-ended <close|message: what ended the hold, the socket closing or the client saying something>, discovered <instance> port=<port> path=<path> name=<name>, service-removed <instance>.

The ramp: left is the low 16 bits of the frame index, right the high 16, so every output frame names its place.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from pathlib import Path

from aiohttp import WSMsgType, web
from aiosendspin.models.types import AudioCodec, PairingCodeFormat, PairMethod
from aiosendspin.noise.keys import Identity
from aiosendspin.noise.pairing import PairingAbortError, PairingAttempt
from aiosendspin.noise.trust_store import FileServerPairingStore, InMemoryServerPairingStore, ServerPairingStore
from aiosendspin.server.audio import AudioFormat
from aiosendspin.server.connection import SendspinConnection
from aiosendspin.server.push_stream import StreamStoppedError
from aiosendspin.server.roles.player.v1 import PlayerV1Role
from aiosendspin.server.roles.source.events import SourceStreamStartedEvent
from aiosendspin.server.server import (
    ClientConnectedEvent,
    ClientDisconnectedEvent,
    SendspinServer,
)
from zeroconf import ServiceStateChange
from zeroconf.asyncio import AsyncServiceInfo

RATE = 48000
CHUNK_FRAMES = 1200  # 25 ms
MAX_AHEAD_US = 1_000_000
PLAYED_OUT_MARGIN_S = 0.2
SERVER_NAME = "ear-test-server"
SOURCE_BYTES_PER_FRAME = 6  # PCM 24 stereo
SOURCE_SAMPLE_SCALE = 8388608.0


def emit(event: str) -> None:
    print(event, flush=True)


class _SentRamp:
    """The ramp's frames as the server hands them to the client, chunk by chunk, with the stamps they carry.

    commit_audio() returns where the server's timeline put a commit then, but the stamps a client gets can differ: when a
    commit comes later than the send-ahead allows (a loaded host), the server moves its whole timeline on, and audio its
    encoder still held from earlier commits goes out on the moved one (push_stream.py's rebase, measured at +7 ms between
    the first and second commit). The frames stay contiguous; only their stamps jump. So the clock test dates each ramp
    frame by the chunk that carried it, as the client did. One stream at a time, delivered live (no catch-up after a
    join), so the chunks follow one another frame for frame.
    """

    frames = 0  # ramp frames handed over so far in this stream

    @classmethod
    def start(cls) -> None:
        cls.frames = 0


_hand_chunk = PlayerV1Role.on_audio_chunk


def _hand_and_say(role: PlayerV1Role, chunk) -> None:  # noqa: ANN001: aiosendspin's AudioChunk
    frames = round(chunk.duration_us * RATE / 1_000_000)
    emit(f"chunk-sent {_SentRamp.frames} {frames} {chunk.timestamp_us}")
    _SentRamp.frames += frames
    _hand_chunk(role, chunk)


PlayerV1Role.on_audio_chunk = _hand_and_say


class _Gap:
    """A network gap on the way to the client: audio frames the server sends are held, after its own late check, and
    then all sent at once, so those whose time has passed reach the client late, as a stalled network delivers them.

    The server drops a chunk already late as it sends it (`_check_late_binary`), so a gap in what it sends never
    delivers one; a gap after that check does. Only player audio is held: time messages go on, so the client's time
    filter is undisturbed. The release is placed against the held chunks' stamps (`TestServer.gap`).
    """

    held: list[tuple] | None = None  # (connection, transport, role, entry, buffer tracker) while held
    arrived: asyncio.Event | None = None  # set as each frame is held
    sent_end_us = 0  # the stamp the last audio frame sent ends at


_send_binary = SendspinConnection._send_binary_data  # noqa: SLF001


async def _send_or_hold(self: SendspinConnection, wsock, role, entry, buffer_tracker) -> None:  # noqa: ANN001
    if entry.binary is not None and entry.binary.player_audio_header:
        if _Gap.held is not None:
            _Gap.held.append((self, wsock, role, entry, buffer_tracker))
            _Gap.arrived.set()
            return
        _Gap.sent_end_us = entry.timestamp_us + (entry.binary.duration_us or 0)
    await _send_binary(self, wsock, role, entry, buffer_tracker)


SendspinConnection._send_binary_data = _send_or_hold  # noqa: SLF001


def ramp(start: int, frames: int) -> bytes:
    pcm = bytearray()
    for index in range(start, start + frames):
        pcm += (index & 0xFFFF).to_bytes(2, "little") + ((index >> 16) & 0xFFFF).to_bytes(2, "little")
    return bytes(pcm)


class DiscoveringServer(SendspinServer):
    """aiosendspin's discovery, deaf to every instance but the test's own, and saying what it saw."""

    discover_prefix = ""

    def _on_mdns_service_state_change(self, zeroconf, service_type, name, state_change) -> None:  # noqa: ANN001
        if not name.startswith(self.discover_prefix):
            return
        instance = name.removesuffix("." + service_type)
        if state_change is ServiceStateChange.Removed:
            emit(f"service-removed {instance}")
        elif state_change is ServiceStateChange.Added:
            self.loop.call_soon_threadsafe(lambda: asyncio.ensure_future(self._say_discovered(zeroconf, service_type, name, instance)))
        super()._on_mdns_service_state_change(zeroconf, service_type, name, state_change)

    async def _say_discovered(self, zeroconf, service_type, name, instance) -> None:  # noqa: ANN001
        info = AsyncServiceInfo(service_type, name)
        if not info.load_from_cache(zeroconf):
            await info.async_request(zeroconf, 3000)
        txt = {key.decode(): (value or b"").decode() for key, value in (info.properties or {}).items()}
        emit(f"discovered {instance} port={info.port} path={txt.get('path')} name={txt.get('name')}")


async def open_state(directory: str | None) -> tuple[Identity, ServerPairingStore]:
    """The server's identity and pairings: a test's own, or what an earlier run left in `directory`."""
    if directory is None:
        return Identity.generate(), InMemoryServerPairingStore()
    folder = Path(directory)
    key_file = folder / "identity"
    if key_file.exists():
        identity = Identity.from_private_bytes(key_file.read_bytes())
    else:
        identity = Identity.generate()
        key_file.write_bytes(identity.private_bytes)
    return identity, await FileServerPairingStore.open(folder / "pairings.json")


def hold_first_activation(released: asyncio.Event) -> None:
    """Every connection waits after its hellos until `released`: aiosendspin has no seam here, so its step is wrapped."""
    activate = SendspinConnection._activate  # noqa: SLF001

    async def held(self: SendspinConnection) -> None:
        if self._declared_activities is not None:  # noqa: SLF001
            await activate(self)
            return
        emit("activate-held")
        wait = asyncio.ensure_future(released.wait())
        heard = asyncio.ensure_future(self._transport.receive())  # noqa: SLF001
        await asyncio.wait((wait, heard), return_when=asyncio.FIRST_COMPLETED)
        if not heard.done():
            heard.cancel()
            await asyncio.gather(heard, return_exceptions=True)
            await activate(self)
            return
        wait.cancel()
        closed = heard.result().type in (WSMsgType.CLOSE, WSMsgType.CLOSING, WSMsgType.CLOSED)
        emit(f"activate-ended {'close' if closed else 'message'}")
        await released.wait()

    SendspinConnection._activate = held  # noqa: SLF001


class TestServer:
    def __init__(
        self,
        loop: asyncio.AbstractEventLoop,
        slow_upgrade: float,
        identity: Identity,
        pairing_store: ServerPairingStore,
        name: str,
    ) -> None:
        self._slow_upgrade = slow_upgrade
        self._client_id: str | None = None
        self._plays: list[asyncio.Task[None]] = []
        self._runner: web.AppRunner | None = None
        self._pairing: asyncio.Task[None] | None = None
        self._codes: asyncio.Queue[str] = asyncio.Queue()
        self._heard: asyncio.Task[None] | None = None
        self._subscribed: set[str] = set()
        # Music Assistant's default: a client off the spec is refused.
        self._server = DiscoveringServer(
            loop,
            identity,
            name,
            pairing_store=pairing_store,
            allow_noncompliant_clients=False,
        )
        self._server.add_event_listener(self._on_event)

    def _on_event(self, _server: SendspinServer, event: object) -> None:
        if isinstance(event, ClientConnectedEvent):
            self._client_id = event.client_id
            self._subscribe(event.client_id)
            emit(f"connected {event.client_id}")
        elif isinstance(event, ClientDisconnectedEvent):
            reason = event.goodbye_reason.value if event.goodbye_reason is not None else "none"
            emit(f"disconnected {reason}")

    def _subscribe(self, client_id: str) -> None:
        """Once per client: a client object outlives its connections."""
        client = self._server.get_client(client_id)
        if client is not None and client_id not in self._subscribed:
            self._subscribed.add(client_id)
            client.add_event_listener(self._on_client_event)

    def _on_client_event(self, _client: object, event: object) -> None:
        if isinstance(event, SourceStreamStartedEvent):
            self._heard = asyncio.ensure_future(self._hear_source(event))
            self._heard.add_done_callback(self._done)

    async def _hear_source(self, started: SourceStreamStartedEvent) -> None:
        """Until the client ends the stream: each chunk as it arrives, and what arrived, in one line at its end."""
        audio_format = started.audio_format
        emit(f"source-started {audio_format.sample_rate} {audio_format.channels} {audio_format.bit_depth}")
        frames = 0
        squares = 0.0
        right_nonzero = 0
        said_heard = False
        async for pcm, timestamp_us in started.handle:
            chunk_frames = len(pcm) // SOURCE_BYTES_PER_FRAME
            emit(f"source-chunk {chunk_frames} {timestamp_us} {self._server.clock.now_us() - timestamp_us}")
            for at in range(0, len(pcm), SOURCE_BYTES_PER_FRAME):
                left = int.from_bytes(pcm[at : at + 3], "little", signed=True) / SOURCE_SAMPLE_SCALE
                squares += left * left
                right_nonzero += any(pcm[at + 3 : at + 6])
            frames += chunk_frames
            if not said_heard and frames >= audio_format.sample_rate:
                said_heard = True
                emit("source-heard")
        rms = (squares / frames) ** 0.5 if frames else 0.0
        emit(f"source-ended {frames} {rms:.6f} {right_nonzero}")

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

    async def discover(self, prefix: str) -> None:
        """start_server, the way Music Assistant finds players: browses mDNS and dials what it finds. Advertises nothing."""
        self._server.discover_prefix = prefix
        await self._server.start_server(port=0, advertise_addresses=[], discover_clients=True)
        port = self._server._app_runner.addresses[0][1]  # noqa: SLF001
        emit(f"ready ws://127.0.0.1:{port}{SendspinServer.API_PATH}")

    def dial(self, url: str) -> None:
        self._server.connect_to_client(url, retry_initial_connection=True)

    def use(self, client_id: str) -> None:
        self._client_id = client_id

    async def trust(self) -> None:
        await self._server.trust_unpaired(self._client_id)
        emit("trusted")

    def say_roles(self) -> None:
        client = self._server.get_client(self._client_id)
        held = [family for family in ("player", "source") if client is not None and client.roles_by_family(family)]
        emit(f"roles {','.join(held) if held else 'none'}")

    async def _pair(self) -> None:
        attempt = PairingAttempt(
            method=PairMethod.DYNAMIC_PAIRING_CODE,
            pairing_code_provider=self._codes.get,
            pairing_format=PairingCodeFormat.DIGITS,
            on_pair_pending=lambda _message: emit("pair-pending"),
        )
        try:
            await self._server.initiate_pairing(self._client_id, attempt)
        except PairingAbortError as err:
            emit(f"pair-failed {err.reason.value}")
        except Exception as err:  # noqa: BLE001  the connection went under it, as a restarted client's does
            emit(f"pair-failed {type(err).__name__}")
        else:
            emit("pair-finished")

    def pair(self) -> None:
        if self._pairing is not None:
            self._pairing.cancel()
        self._codes = asyncio.Queue()
        self._pairing = asyncio.ensure_future(self._pair())
        self._pairing.add_done_callback(self._done)

    def code(self, digits: str) -> None:
        self._codes.put_nowait(digits)

    def prefer_16(self) -> None:
        role = self._server.get_client(self._client_id).roles_by_family("player")[0]
        taken = role.set_preferred_format(AudioFormat(RATE, 16, 2), AudioCodec.FLAC)
        emit(f"prefer-16 {str(taken).lower()}")

    async def say_lead(self) -> None:
        client = await self._player_active(self._client_id)
        emit(f"lead {client.roles_by_family('player')[0].required_lead_time_ms}")

    def _source_role(self):
        client = self._server.get_client(self._client_id)
        return client.roles_by_family("source")[0]

    async def _player_active(self, client_id: str):
        """Until the client's player role takes audio: `trust` activates it, and the client answers in its own time.

        The library has no event for this; a stream started before it would join the ramp late, part way in.
        """
        client = self._server.get_client(client_id)
        while client is None or client.awaits_role_state("player") or not client.roles_by_family("player"):
            await asyncio.sleep(0.005)
            client = self._server.get_client(client_id)
        return client

    async def _play_ramp(self, client_id: str, seconds: float) -> None:
        group = (await self._player_active(client_id)).group
        _SentRamp.start()
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
        play = asyncio.ensure_future(self._play_ramp(self._client_id, seconds))
        play.add_done_callback(self._done)
        self._plays.append(play)

    @staticmethod
    def _done(task: asyncio.Task[None]) -> None:
        if not task.cancelled() and task.exception() is not None:
            logging.error("a task failed", exc_info=task.exception())

    async def _gap(self, starve_ms: float) -> None:
        """Holds the audio the server sends until the client has had nothing to play for about `starve_ms`, then sends
        it all at once (_Gap). The release falls inside a held chunk, past its start by a third of what lies between 5 ms
        in from either end (the library's hard-sync threshold, sync_task.cpp), so the chunk the client is then playing
        is late at its start and not at its end: its front is late, the chunks before it wholly so.
        """
        clock = self._server.clock
        _Gap.held = []
        _Gap.arrived = asyncio.Event()
        starved_from = _Gap.sent_end_us
        emit(f"gap-held {starved_from}")
        due = starved_from + int(starve_ms * 1000)
        target = None
        looked = 0
        while target is None:
            await _Gap.arrived.wait()
            _Gap.arrived.clear()
            for entry in [held[3] for held in _Gap.held[looked:]]:
                looked += 1
                duration = entry.binary.duration_us or 0
                at = entry.timestamp_us + 5000 + (duration - 10000) // 3
                if entry.timestamp_us >= due - duration and at >= clock.now_us():
                    target = (entry.timestamp_us, duration, at)
                    break
        stamp, duration, at = target
        await asyncio.sleep(max(at - clock.now_us(), 0) / 1_000_000)
        count = 0
        while _Gap.held:
            connection, wsock, role, entry, buffer_tracker = _Gap.held.pop(0)
            _Gap.sent_end_us = entry.timestamp_us + (entry.binary.duration_us or 0)
            await _send_binary(connection, wsock, role, entry, buffer_tracker)
            count += 1
            if count == 1:
                released_at = clock.now_us()
        _Gap.held = None
        emit(f"gap-released {count} {stamp} {duration} {at} {released_at}")

    def gap(self, starve_ms: float) -> None:
        task = asyncio.ensure_future(self._gap(starve_ms))
        task.add_done_callback(self._done)
        self._plays.append(task)

    def end(self) -> None:
        for play in self._plays:
            play.cancel()
        self._plays = []
        client = self._server.get_client(self._client_id)
        if client is not None:
            client.group.stop_stream()

    async def close(self) -> None:
        for task in (*self._plays, self._pairing, self._heard):
            if task is not None:
                task.cancel()
        await self._server.close()
        if self._runner is not None:
            await self._runner.cleanup()


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", type=int, required=True)
    parser.add_argument("--slow-upgrade", type=float, default=0.0)
    parser.add_argument("--dial")
    parser.add_argument("--state")
    parser.add_argument("--discover")
    parser.add_argument("--name", default=SERVER_NAME)
    parser.add_argument("--hold-activate", action="store_true")
    arguments = parser.parse_args()

    loop = asyncio.get_running_loop()
    activate_released = asyncio.Event()
    if arguments.hold_activate:
        hold_first_activation(activate_released)
    identity, pairing_store = await open_state(arguments.state)
    server = TestServer(loop, arguments.slow_upgrade, identity, pairing_store, arguments.name)
    if arguments.discover is not None:
        await server.discover(arguments.discover)
    else:
        await server.listen()
    if arguments.dial is not None:
        server.dial(arguments.dial)

    reader = asyncio.StreamReader()
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
    while line := await reader.readline():
        command, *rest = line.decode().split()
        if command == "dial":
            server.dial(rest[0])
        elif command == "activate-release":
            activate_released.set()
        elif command == "use":
            server.use(rest[0])
        elif command == "trust":
            await server.trust()
        elif command == "roles":
            server.say_roles()
        elif command == "prefer-16":
            server.prefer_16()
        elif command == "lead":
            await server.say_lead()
        elif command == "pair":
            server.pair()
        elif command == "code":
            server.code(rest[0])
        elif command == "source-start":
            server._source_role().request_start()
        elif command == "source-stop":
            server._source_role().request_stop()
        elif command == "play":
            server.play(float(rest[0]))
        elif command == "gap":
            server.gap(float(rest[0]))
        elif command == "end":
            server.end()
        elif command == "quit":
            break
    await server.close()


if __name__ == "__main__":
    logging.basicConfig(stream=sys.stderr, level=logging.WARNING)
    asyncio.run(main())

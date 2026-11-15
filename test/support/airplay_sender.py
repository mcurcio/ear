#!/usr/bin/env python3
"""The airplay node's scripted AirPlay 1 sender (docs/testing.md#the-airplay-sender): one JSON event per stdout line, so
a test moves when an event arrives. Run by the image's /opt/airplay-sender venv (test/airplay/requirements.txt).

--browse <speaker>: a zeroconf browser on _raop._tcp.local. over multicast, as a phone's is. It emits
{"event":"discovered","address":...,"port":...,"txt":{...}} when an instance whose name ends in "@<speaker>" is added
(a RAOP instance is <device id>@<speaker name>), and {"event":"removed"} when that instance goes. It runs until
SIGTERM or SIGINT.

--play <wav> --address A --port P --txt JSON [--speaker S] [--volume V]: pyatv streams the file to that speaker as
AirPlay 1 (RAOP), given the listing's address, port and TXT records as found, so pyatv does no discovery of its own. It
sets the sender's volume first, as a phone's slider: V from 0 to 100 (default 100, the top). The airplay node tells
shairport-sync to ignore it, so a test sends a low one to prove that. It emits {"event":"started"} once connected and
{"event":"finished"} once the file has been sent, then exits 0; any failure is {"event":"failed","why":...} and exit 1.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import signal
import sys

RAOP = "_raop._tcp.local."


def emit(event: str, **fields: object) -> None:
    print(json.dumps({"event": event, **fields}), flush=True)


async def until_signalled() -> None:
    """Returns once SIGTERM or SIGINT arrives: the test's way to end a browse."""
    loop = asyncio.get_running_loop()
    stop = asyncio.Event()
    for number in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(number, stop.set)
    await stop.wait()


async def browse(speaker: str) -> int:
    from zeroconf import IPVersion, ServiceStateChange
    from zeroconf.asyncio import AsyncServiceBrowser, AsyncServiceInfo, AsyncZeroconf

    suffix = f"@{speaker}.{RAOP}"
    zeroconf = AsyncZeroconf(ip_version=IPVersion.V4Only)
    found: set[str] = set()
    pending: set[asyncio.Task[None]] = set()

    async def resolve(name: str) -> None:
        info = AsyncServiceInfo(RAOP, name)
        if not await info.async_request(zeroconf.zeroconf, 3000):
            emit("unresolved", instance=name)
            return
        addresses = info.parsed_addresses(IPVersion.V4Only)
        txt = {
            key.decode("utf-8", "replace"): (value or b"").decode("utf-8", "replace")
            for key, value in info.properties.items()
        }
        found.add(name)
        emit("discovered", instance=name, address=addresses[0] if addresses else "", port=info.port, txt=txt)

    def changed(zeroconf: object, service_type: str, name: str, state_change: ServiceStateChange) -> None:
        if not name.endswith(suffix):
            return
        if state_change is ServiceStateChange.Added:
            task = asyncio.ensure_future(resolve(name))
            pending.add(task)
            task.add_done_callback(pending.discard)
        elif state_change is ServiceStateChange.Removed and name in found:
            found.discard(name)
            emit("removed", instance=name)

    browser = AsyncServiceBrowser(zeroconf.zeroconf, RAOP, handlers=[changed])
    emit("browsing", service=RAOP, speaker=speaker)
    try:
        await until_signalled()
    finally:
        await browser.async_cancel()
        await zeroconf.async_close()
    return 0


async def play(wav: str, address: str, port: int, txt: dict[str, str], speaker: str, volume: float) -> int:
    import pyatv
    from pyatv import conf
    from pyatv.const import Protocol

    config = conf.AppleTV(address, speaker)
    # pyatv refuses a device with no identifier ("no device identifier"); the speaker's address and port are one.
    config.add_service(conf.ManualService(f"{address}:{port}", Protocol.RAOP, port, properties=txt))
    try:
        atv = await pyatv.connect(config, asyncio.get_running_loop())
    except Exception as error:  # pyatv's own errors and the socket's: the test reads why
        emit("failed", why=f"connect: {error!r}")
        return 1
    try:
        emit("started")
        await atv.audio.set_volume(volume)
        await atv.stream.stream_file(wav)
        emit("finished")
        return 0
    except Exception as error:
        emit("failed", why=f"stream: {error!r}")
        return 1
    finally:
        await asyncio.gather(*atv.close())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--browse", metavar="SPEAKER")
    parser.add_argument("--play", metavar="WAV")
    parser.add_argument("--address")
    parser.add_argument("--port", type=int)
    parser.add_argument("--txt", default="{}")
    parser.add_argument("--speaker", default="ear")
    parser.add_argument("--volume", type=float, default=100.0)
    args = parser.parse_args()
    if not 0.0 <= args.volume <= 100.0:
        parser.error("--volume is 0 to 100")
    if args.browse:
        return asyncio.run(browse(args.browse))
    if args.play and args.address and args.port:
        return asyncio.run(
            play(args.play, args.address, args.port, json.loads(args.txt), args.speaker, args.volume)
        )
    parser.error("give --browse <speaker>, or --play <wav> --address <a> --port <p> [--txt <json>]")
    return 2


if __name__ == "__main__":
    sys.exit(main())

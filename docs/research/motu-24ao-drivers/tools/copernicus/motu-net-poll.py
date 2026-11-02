#!/usr/bin/python3 -I
"""Polls the MOTU 24Ao's own web server over Ethernet the way its web app (the dashboard) does:
a long-poll loop on GET /datastore with If-None-Match (the server answers when something changes
or after its own timeout), plus GET /apiversion every 30 s. Logs every request with Pacific and UTC
time to the log file, and stops at the first failure (timeout, refused, non-2xx/304).

usage: motu-net-poll.py HOST LOGFILE
"""
import datetime, sys, threading, time, urllib.error, urllib.request
from zoneinfo import ZoneInfo

HOST, LOG = sys.argv[1], sys.argv[2]
PT = ZoneInfo("America/Los_Angeles")
stop = threading.Event()
lock = threading.Lock()


def log(msg):
    now = datetime.datetime.now(datetime.timezone.utc)
    line = "%s PT (%s UTC) %s" % (now.astimezone(PT).strftime("%H:%M:%S"), now.strftime("%H:%M:%S"), msg)
    with lock:
        print(line, flush=True)
        open(LOG, "a").write(line + "\n")


def get(path, headers=None, timeout=30):
    req = urllib.request.Request("http://%s%s" % (HOST, path), headers=headers or {})
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read()
            return r.status, r.headers.get("ETag"), len(body), time.monotonic() - t0
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("ETag"), 0, time.monotonic() - t0


def fail(what, err):
    log("FAIL %s: %r" % (what, err))
    stop.set()


def longpoll():
    etag = None
    while not stop.is_set():
        try:
            h = {"If-None-Match": etag} if etag else {}
            st, new, n, dt = get("/datastore?client=4242", h, timeout=75)
            log("datastore %s %dB %.2fs etag=%s" % (st, n, dt, new))
            if st not in (200, 304):
                return fail("datastore", st)
            etag = new or etag
        except Exception as e:
            return fail("datastore", e)


def heartbeat():
    while not stop.is_set():
        try:
            st, _, n, dt = get("/apiversion", timeout=20)
            log("apiversion %s %.3fs" % (st, dt))
            if st != 200:
                return fail("apiversion", st)
        except Exception as e:
            return fail("apiversion", e)
        stop.wait(30)


log("start polling http://%s like the web app" % HOST)
threading.Thread(target=longpoll, daemon=True).start()
threading.Thread(target=heartbeat, daemon=True).start()
stop.wait()
sys.exit(1)

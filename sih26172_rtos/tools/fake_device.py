#!/usr/bin/env python3
"""
Fake ESP32-S3 device: streams audio to asr_server.py exactly like main/net_stream.c.

Sequence:
  1. connect to ws://... or wss://...
  2. TEXT start message (same fields and formatting as the firmware)
  3. BINARY pre-roll burst: 300 ms of quiet background, sent at once in frames of up to 100 ms
  4. BINARY live audio in real time, one 20 ms frame every 20 ms:
     a tone ("speech") followed by near silence ("trailing silence")
  5. TEXT stop message, then a clean WebSocket close

Any text the server sends back is printed. If it contains "end_of_speech" the fake
device stops early with reason "server_end_of_speech", like the firmware does.

Examples:
  python3 fake_device.py
  python3 fake_device.py --uri ws://192.168.1.50:8765/stream
  python3 fake_device.py --uri wss://127.0.0.1:8765/stream --cafile certs/server_cert.pem
  python3 fake_device.py --uri wss://127.0.0.1:8765/stream --insecure
  python3 fake_device.py --drop-after-ms 800     # vanish without a stop message
"""

import argparse
import asyncio
import math
import random
import ssl
import sys
import time
from array import array

try:
    from websockets.asyncio.client import connect
    from websockets.exceptions import ConnectionClosed
except ImportError:
    sys.exit("This script needs websockets>=13. Install it with: pip install -r requirements.txt")

SAMPLE_RATE = 16000
SAMPLES_PER_MS = SAMPLE_RATE // 1000
MAX_FRAME_MS = 100   # firmware default CONFIG_SIH_WS_MAX_FRAME_MS (pre-roll burst frame size)


class Signal:
    """Synthetic microphone: a sine tone for the speech part, low noise otherwise."""

    def __init__(self, tone_hz, level_dbfs):
        self.phase = 0.0
        self.step = 2 * math.pi * tone_hz / SAMPLE_RATE
        self.amp = 32767 * 10 ** (level_dbfs / 20.0)
        self.rng = random.Random(1234)

    def chunk(self, n, tone):
        out = array("h")
        for _ in range(n):
            v = self.rng.gauss(0, 20)            # about -64 dBFS background noise
            if tone:
                v += self.amp * math.sin(self.phase)
                self.phase += self.step
            out.append(max(-32768, min(32767, int(v))))
        if sys.byteorder != "little":
            out.byteswap()                        # wire format is little-endian
        return out.tobytes()


def make_ssl(args):
    if not args.uri.startswith("wss://"):
        return None
    if args.insecure:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx
    if args.cafile:
        # Trust only this certificate, and skip the host name check like the firmware
        # does (skip_cert_common_name_check), since the laptop is addressed by IP.
        ctx = ssl.create_default_context(cafile=args.cafile)
        ctx.check_hostname = False
        return ctx
    return ssl.create_default_context()           # public server with a real certificate


async def receiver(ws, eos):
    """Print everything the server sends; flag end_of_speech."""
    try:
        async for msg in ws:
            if isinstance(msg, bytes):
                print("server: <%d binary bytes>" % len(msg))
                continue
            print("server: %s" % msg)
            if "end_of_speech" in msg:
                eos.set()
    except ConnectionClosed:
        pass


async def run(args):
    sig = Signal(args.tone_hz, args.level_dbfs)
    t0 = time.monotonic()
    async with connect(args.uri, ssl=make_ssl(args), compression=None, open_timeout=6) as ws:
        print("connected to %s in %.0f ms" % (args.uri, (time.monotonic() - t0) * 1000))
        eos = asyncio.Event()
        rx = asyncio.create_task(receiver(ws, eos))

        # 1. start message, same text layout as the firmware's snprintf
        wake_us = int(time.monotonic() * 1e6)
        start = ('{"type":"start","session":%d,"sample_rate":%d,"format":"s16le",'
                 '"channels":1,"preroll_ms":%d,"wake_confidence":%.3f,"wake_timestamp_us":%d}'
                 % (args.session, SAMPLE_RATE, args.preroll_ms, 1.0, wake_us))
        await ws.send(start)
        print("sent: %s" % start)

        sent = 0      # samples sent
        frames = 0

        # 2. pre-roll burst: everything at once, split into frames of up to 100 ms
        left = args.preroll_ms * SAMPLES_PER_MS
        while left > 0:
            n = min(left, MAX_FRAME_MS * SAMPLES_PER_MS)
            await ws.send(sig.chunk(n, tone=False))
            left -= n
            sent += n
            frames += 1
        print("sent pre-roll burst: %d ms in %d frames" % (args.preroll_ms, frames))

        # 3. live audio, paced to the wall clock so it arrives in real time
        frame_n = args.frame_ms * SAMPLES_PER_MS
        live_total = (args.speech_ms + args.silence_ms) * SAMPLES_PER_MS
        live_sent = 0
        t_live = time.monotonic()
        while live_sent < live_total:
            if eos.is_set():
                break           # server asked us to stop (firmware: server_end_of_speech)
            if args.drop_after_ms is not None and sent >= args.drop_after_ms * SAMPLES_PER_MS:
                print("dropping the connection now (no stop message, no close)")
                ws.transport.abort()
                rx.cancel()
                return
            n = min(frame_n, live_total - live_sent)
            is_speech = live_sent < args.speech_ms * SAMPLES_PER_MS
            # Wait until this frame's audio would exist on a real mic.
            ready_at = t_live + (live_sent + n) / SAMPLE_RATE
            await asyncio.sleep(max(0.0, ready_at - time.monotonic()))
            await ws.send(sig.chunk(n, tone=is_speech))
            live_sent += n
            sent += n
            frames += 1
        reason = "server_end_of_speech" if eos.is_set() else "trailing_silence"

        # 4. stop message and clean close
        stop = '{"type":"stop","reason":"%s","audio_ms":%d}' % (reason, sent // SAMPLES_PER_MS)
        await ws.send(stop)
        print("sent: %s" % stop)
        await ws.close()
        await rx
    print("done: %d ms of audio in %d frames, reason %s" % (sent // SAMPLES_PER_MS, frames, reason))


def main():
    ap = argparse.ArgumentParser(description="Fake SIH26172 device that streams test audio to asr_server.py.")
    ap.add_argument("--uri", default="ws://127.0.0.1:8765/stream", help="server URI (default %(default)s)")
    ap.add_argument("--session", type=int, default=1, help="session number in the start message")
    ap.add_argument("--preroll-ms", type=int, default=300, help="pre-roll burst length (default 300)")
    ap.add_argument("--frame-ms", type=int, default=20, help="live frame size (default 20)")
    ap.add_argument("--speech-ms", type=int, default=1500, help="tone length after the trigger (default 1500)")
    ap.add_argument("--silence-ms", type=int, default=1000, help="trailing silence length (default 1000)")
    ap.add_argument("--tone-hz", type=float, default=440.0, help="tone frequency (default 440)")
    ap.add_argument("--level-dbfs", type=float, default=-12.0, help="tone peak level in dBFS (default -12)")
    ap.add_argument("--drop-after-ms", type=int, metavar="N",
                    help="cut the connection after N ms of audio without a stop message")
    ap.add_argument("--cafile", help="wss://: trust this certificate (e.g. certs/server_cert.pem)")
    ap.add_argument("--insecure", action="store_true", help="wss://: do not check the certificate at all")
    args = ap.parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except (OSError, ConnectionClosed, asyncio.TimeoutError) as e:
        sys.exit("error: %s" % e)


if __name__ == "__main__":
    main()

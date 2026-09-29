#!/usr/bin/env python3
"""
Laptop-side WebSocket test server for the SIH26172 ESP32-S3 firmware.

What the device sends (see main/net_stream.c):
  1. TEXT   {"type":"start","session":N,"sample_rate":16000,"format":"s16le",
             "channels":1,"preroll_ms":300,"wake_confidence":1.000,"wake_timestamp_us":123}
  2. BINARY raw PCM, signed 16-bit little-endian, 16 kHz, mono. First a quick burst
            (pre-roll plus any backlog), then small frames in real time.
  3. TEXT   {"type":"stop","reason":"trailing_silence","audio_ms":2840}, then a clean close.

What this server can send back (TEXT):
  {"type":"partial","text":...} / {"type":"final","text":...}   transcripts (with --vosk-model)
  {"type":"end_of_speech"}                                      asks the device to stop early

For every session it saves a WAV file plus a JSON file with the start/stop messages
and prints timing and level numbers so you can check the device end to end.

Examples:
  python3 asr_server.py
  python3 asr_server.py --server-eos-after-ms 1500
  python3 asr_server.py --vosk-model ~/models/vosk-model-small-en-us-0.15 --server-eos
  python3 asr_server.py --certfile certs/server_cert.pem --keyfile certs/server_key.pem

Needs Python 3.9+ and websockets>=13 (pip install -r requirements.txt).
"""

import argparse
import asyncio
import datetime
import json
import math
import os
import signal
import socket
import ssl
import sys
import time
import wave
from array import array

try:
    from websockets.asyncio.server import serve
    from websockets.exceptions import ConnectionClosed
except ImportError:
    sys.exit("This script needs websockets>=13. Install it with: pip install -r requirements.txt")

SAMPLE_RATE = 16000
BYTES_PER_SAMPLE = 2
EXPECTED_PATH = "/stream"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


# --------------------------------------------------------------------- helpers
def lan_ip():
    """Best-effort LAN IP of this laptop.

    Connecting a UDP socket sends no packets. It only asks the OS which local
    address it would use to reach the internet, which is normally the Wi-Fi IP.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def dbfs(value):
    """Convert a 16-bit sample magnitude to dBFS (0 dBFS = full scale)."""
    if value <= 0:
        return float("-inf")
    return 20.0 * math.log10(value / 32768.0)


def fmt_db(value):
    return "-inf" if value == float("-inf") else "%.1f" % value


def unique_path(base):
    """Return base if no .wav/.json with that name exists yet, else base_2, base_3 ..."""
    candidate, n = base, 2
    while os.path.exists(candidate + ".wav") or os.path.exists(candidate + ".json"):
        candidate = "%s_%d" % (base, n)
        n += 1
    return candidate


# --------------------------------------------------------------------- session
class Session:
    """State and statistics for one WebSocket connection."""

    def __init__(self, conn_id, peer, path, scheme, out_dir):
        self.conn_id = conn_id
        self.peer = peer
        self.path = path
        self.scheme = scheme
        self.out_dir = out_dir
        self.t_open = time.monotonic()
        self.opened_at = datetime.datetime.now()

        self.start_msg = None
        self.stop_msg = None
        self.t_start = None          # monotonic time the start msg arrived
        self.t_first_audio = None    # monotonic time the first binary frame arrived

        self.frames = []             # (arrival time, samples in frame) per binary frame
        self.total_samples = 0
        self.odd_byte = b""          # carry for a frame with an odd byte count

        self.peak = 0
        self.sum_sq = 0.0

        self.wav = None
        self.base_path = None

        self.eos_sent_at_ms = None   # audio position when we sent end_of_speech
        self.transcript = []         # final texts from Vosk
        self.close_info = None       # text description of how the connection ended

    # ---- naming and files
    @property
    def label(self):
        if self.start_msg and "session" in self.start_msg:
            return "#%s" % self.start_msg["session"]
        return "conn %d" % self.conn_id

    def open_wav(self):
        stamp = self.opened_at.strftime("%Y%m%d-%H%M%S")
        if self.start_msg and isinstance(self.start_msg.get("session"), int):
            name = "%s_session%04d" % (stamp, self.start_msg["session"])
        else:
            name = "%s_conn%d_nostart" % (stamp, self.conn_id)
        self.base_path = unique_path(os.path.join(self.out_dir, name))
        self.wav = wave.open(self.base_path + ".wav", "wb")
        self.wav.setnchannels(1)
        self.wav.setsampwidth(BYTES_PER_SAMPLE)
        self.wav.setframerate(SAMPLE_RATE)

    # ---- audio
    def add_audio(self, data):
        now = time.monotonic()
        if self.t_first_audio is None:
            self.t_first_audio = now
        if self.wav is None:
            self.open_wav()

        data = self.odd_byte + data
        if len(data) % 2:
            self.odd_byte, data = data[-1:], data[:-1]
        else:
            self.odd_byte = b""
        n = len(data) // 2
        if n == 0:
            return

        self.wav.writeframes(data)
        self.frames.append((now, n))
        self.total_samples += n

        # Level stats. array("h") uses the machine byte order, so swap on big-endian hosts.
        samples = array("h")
        samples.frombytes(data)
        if sys.byteorder != "little":
            samples.byteswap()
        self.peak = max(self.peak, max(abs(v) for v in samples))
        self.sum_sq += float(sum(v * v for v in samples))

    @property
    def audio_ms(self):
        return self.total_samples * 1000 // SAMPLE_RATE

    # ---- report
    def arrival_stats(self):
        """Split frames into the initial burst and the live part and measure each.

        The burst ends at the first frame that arrives after a pause of at least half
        its own duration, i.e. when frames stop coming back to back.
        """
        frames = self.frames
        k = len(frames)
        for i in range(1, len(frames)):
            gap = frames[i][0] - frames[i - 1][0]
            if gap >= 0.5 * frames[i][1] / SAMPLE_RATE:
                k = i
                break
        burst_samples = sum(n for _, n in frames[:k])
        burst_wall = frames[k - 1][0] - frames[0][0] if k > 0 else 0.0
        live_samples = sum(n for _, n in frames[k:])
        live_wall = frames[-1][0] - frames[k - 1][0] if k < len(frames) else 0.0

        max_gap, max_gap_at = 0.0, 0
        pos = 0
        for i in range(len(frames)):
            if i > 0:
                gap = frames[i][0] - frames[i - 1][0]
                if gap > max_gap:
                    max_gap, max_gap_at = gap, pos
            pos += frames[i][1]
        return {
            "burst_frames": k,
            "burst_audio_ms": burst_samples * 1000 // SAMPLE_RATE,
            "burst_wall_ms": round(burst_wall * 1000, 1),
            "live_frames": len(frames) - k,
            "live_audio_s": live_samples / SAMPLE_RATE,
            "live_wall_s": live_wall,
            "max_gap_ms": round(max_gap * 1000, 1),
            "max_gap_at_audio_ms": max_gap_at * 1000 // SAMPLE_RATE,
        }

    def finish(self):
        """Close the WAV, write the JSON, print the report."""
        if self.wav is not None:
            self.wav.close()
        elif self.start_msg is not None:
            self.open_wav()      # start msg but no audio: still save an empty WAV
            self.wav.close()

        rms = math.sqrt(self.sum_sq / self.total_samples) if self.total_samples else 0.0
        stats = self.arrival_stats() if self.frames else None
        sizes = [n for _, n in self.frames]

        ms = lambda t: None if t is None else round((t - self.t_open) * 1000, 1)  # noqa: E731
        summary = {
            "peer": self.peer,
            "path": self.path,
            "scheme": self.scheme,
            "opened_at": self.opened_at.isoformat(timespec="milliseconds"),
            "open_to_start_ms": ms(self.t_start),
            "open_to_first_audio_ms": ms(self.t_first_audio),
            "audio_ms_received": self.audio_ms,
            "frames": len(self.frames),
            "peak_dbfs": None if self.peak == 0 else round(dbfs(self.peak), 2),
            "rms_dbfs": None if rms == 0 else round(dbfs(rms), 2),
            "arrival": stats,
            "server_end_of_speech_at_audio_ms": self.eos_sent_at_ms,
            "transcript": " ".join(self.transcript),
            "connection_end": self.close_info,
        }
        if self.base_path is not None:
            with open(self.base_path + ".json", "w") as f:
                json.dump({"start": self.start_msg, "stop": self.stop_msg, "server": summary}, f, indent=2)

        # ---- printed report
        p = print
        p("")
        p("=" * 64)
        p("Session %s from %s  (%s://...%s)" % (self.label, self.peer, self.scheme, self.path))
        p("-" * 64)
        if self.start_msg is None:
            p("  WARNING: no start message received")
        else:
            p("  open -> start msg        : %s ms" % summary["open_to_start_ms"])
        if self.t_first_audio is None:
            p("  WARNING: no audio received")
        else:
            p("  open -> first audio byte : %s ms" % summary["open_to_first_audio_ms"])
            p("  audio received           : %.3f s in %d frames (frame size %d to %d ms)"
              % (self.total_samples / SAMPLE_RATE, len(self.frames),
                 min(sizes) * 1000 // SAMPLE_RATE, max(sizes) * 1000 // SAMPLE_RATE))
            p("  initial burst            : %d ms of audio in %d frames over %.1f ms"
              % (stats["burst_audio_ms"], stats["burst_frames"], stats["burst_wall_ms"]))
            if stats["live_wall_s"] >= 0.2:
                ratio = stats["live_audio_s"] / stats["live_wall_s"]
                if 0.9 <= ratio <= 1.1:
                    verdict = "OK, real time"
                elif ratio > 1.1:
                    verdict = "faster than real time (still sending backlog?)"
                else:
                    verdict = "SLOWER than real time (Wi-Fi or device falling behind)"
                p("  live arrival rate        : %.3f s audio in %.3f s wall = %.2fx  %s"
                  % (stats["live_audio_s"], stats["live_wall_s"], ratio, verdict))
            else:
                p("  live arrival rate        : too little live audio to judge")
            p("  max gap between frames   : %.1f ms (at %d ms of audio)"
              % (stats["max_gap_ms"], stats["max_gap_at_audio_ms"]))
            note = ""
            if self.peak == 0:
                note = "  <- all zeros, mic not working?"
            elif dbfs(self.peak) < -50:
                note = "  <- very quiet, check the mic"
            elif self.peak >= 32767:
                note = "  <- clipping"
            p("  level                    : peak %s dBFS, RMS %s dBFS%s"
              % (fmt_db(dbfs(self.peak)), fmt_db(dbfs(rms)), note))
        if self.stop_msg is not None:
            dev_ms = self.stop_msg.get("audio_ms")
            match = ""
            if isinstance(dev_ms, int) and dev_ms != self.audio_ms:
                match = "  <- MISMATCH, server got %d ms" % self.audio_ms
            p("  stop reason              : %s (device sent %s ms)%s"
              % (self.stop_msg.get("reason"), dev_ms, match))
        else:
            p("  stop reason              : NO stop message (%s)" % self.close_info)
        if self.eos_sent_at_ms is not None:
            p("  server end_of_speech     : sent at %d ms of audio" % self.eos_sent_at_ms)
        if self.transcript:
            p("  transcript               : %s" % " ".join(self.transcript))
        if self.base_path is not None:
            p("  saved                    : %s.wav (+ .json)" % self.base_path)
        p("=" * 64)
        sys.stdout.flush()


# --------------------------------------------------------------------- server
class AsrServer:
    def __init__(self, args):
        self.args = args
        self.conn_count = 0
        self.shutting_down = False
        self.vosk_model = None
        if args.vosk_model:
            # Imported only when asked for, so the server runs without Vosk installed.
            try:
                import vosk
            except ImportError:
                sys.exit("--vosk-model given but vosk is not installed (pip install vosk)")
            if not os.path.isdir(args.vosk_model):
                sys.exit("Vosk model folder not found: %s" % args.vosk_model)
            vosk.SetLogLevel(-1)
            print("Loading Vosk model from %s ..." % args.vosk_model)
            self.vosk = vosk
            try:
                self.vosk_model = vosk.Model(args.vosk_model)
            except Exception as e:
                sys.exit("Could not load the Vosk model: %s" % e)

    async def send(self, ws, sess, obj):
        """Send a JSON text frame to the device; ignore errors if it already left."""
        text = json.dumps(obj)
        try:
            await ws.send(text)
            print("[%s] server -> device: %s" % (sess.label, text))
        except ConnectionClosed:
            pass

    async def send_eos(self, ws, sess):
        if sess.eos_sent_at_ms is None:
            sess.eos_sent_at_ms = sess.audio_ms
            await self.send(ws, sess, {"type": "end_of_speech"})

    async def handler(self, ws):
        self.conn_count += 1
        path = ws.request.path if ws.request is not None else "?"
        peer = "%s:%s" % ws.remote_address[:2] if ws.remote_address else "?"
        sess = Session(self.conn_count, peer, path, "wss" if self.args.certfile else "ws", self.args.out_dir)
        print("\n[%s] connected from %s, path %s" % (sess.label, peer, path))
        if path != EXPECTED_PATH:
            print("[%s] note: device normally uses path %s" % (sess.label, EXPECTED_PATH))

        rec = None
        last_partial = ""
        heard_text = False
        if self.vosk_model is not None:
            rec = self.vosk.KaldiRecognizer(self.vosk_model, SAMPLE_RATE)

        try:
            async for msg in ws:
                if isinstance(msg, bytes):
                    sess.add_audio(msg)

                    # Test path without Vosk: fixed-length end_of_speech.
                    if self.args.server_eos_after_ms is not None and sess.audio_ms >= self.args.server_eos_after_ms:
                        await self.send_eos(ws, sess)

                    if rec is not None:
                        # Vosk is CPU heavy, so run it in a thread to keep the socket responsive.
                        done = await asyncio.to_thread(rec.AcceptWaveform, msg)
                        if done:
                            text = json.loads(rec.Result()).get("text", "")
                            last_partial = ""
                            if text:
                                heard_text = True
                                sess.transcript.append(text)
                                await self.send(ws, sess, {"type": "final", "text": text})
                            if self.args.server_eos and heard_text:
                                await self.send_eos(ws, sess)
                        else:
                            partial = json.loads(rec.PartialResult()).get("partial", "")
                            if partial and partial != last_partial:
                                heard_text = True
                                last_partial = partial
                                await self.send(ws, sess, {"type": "partial", "text": partial})
                    continue

                # Text frame from the device.
                try:
                    obj = json.loads(msg)
                except ValueError:
                    print("[%s] text (not JSON): %r" % (sess.label, msg[:200]))
                    continue
                kind = obj.get("type") if isinstance(obj, dict) else None
                if kind == "start":
                    sess.start_msg = obj
                    sess.t_start = time.monotonic()
                    print("[%s] start: %s" % (sess.label, msg))
                    if obj.get("sample_rate") != SAMPLE_RATE or obj.get("format") != "s16le" or obj.get("channels") != 1:
                        print("[%s] WARNING: unexpected audio format, WAV assumes 16 kHz s16le mono" % sess.label)
                elif kind == "stop":
                    sess.stop_msg = obj
                    print("[%s] stop: %s" % (sess.label, msg))
                else:
                    print("[%s] text: %s" % (sess.label, msg))
            if self.shutting_down:
                sess.close_info = "server shut down"
            else:
                sess.close_info = "closed cleanly, code %s" % ws.close_code
        except ConnectionClosed as e:
            sess.close_info = "connection dropped (%s)" % (e,)
        except asyncio.CancelledError:
            sess.close_info = "server shutting down"
            raise
        finally:
            try:
                # Flush whatever Vosk still holds (the device may already be gone).
                if rec is not None:
                    text = json.loads(rec.FinalResult()).get("text", "")
                    if text:
                        sess.transcript.append(text)
                        await self.send(ws, sess, {"type": "final", "text": text})
            finally:
                sess.finish()   # always save the WAV/JSON and print the report


def print_banner(args):
    scheme = "wss" if args.certfile else "ws"
    print("")
    print("SIH26172 ASR test server")
    print("  listening on %s:%d (%s)" % (args.host, args.port, "TLS" if args.certfile else "no TLS"))
    print("  saving sessions to %s" % args.out_dir)
    if args.vosk_model:
        print("  Vosk model: %s%s" % (args.vosk_model, ", sends end_of_speech" if args.server_eos else ""))
    if args.server_eos_after_ms is not None:
        print("  will send end_of_speech after %d ms of audio" % args.server_eos_after_ms)
    print("")
    print("Put this in menuconfig -> Streaming server (WebSocket) -> WebSocket URI:")
    if args.host in ("0.0.0.0", "", "::"):
        ip = lan_ip()
        if ip:
            print("    %s://%s:%d%s" % (scheme, ip, args.port, EXPECTED_PATH))
        else:
            print("    (could not detect the LAN IP; use this laptop's Wi-Fi IP)")
        print("  local test (fake_device.py on this laptop):")
        print("    %s://127.0.0.1:%d%s" % (scheme, args.port, EXPECTED_PATH))
    else:
        print("    %s://%s:%d%s" % (scheme, args.host, args.port, EXPECTED_PATH))
    if args.certfile:
        print("  (firmware: Server certificate check = Pinned certificate)")
    print("")
    print("Waiting for the device. Press Ctrl-C to quit.")
    sys.stdout.flush()


async def run(args):
    server = AsrServer(args)

    ssl_ctx = None
    if args.certfile:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(args.certfile, args.keyfile)

    # Stop cleanly on Ctrl-C / SIGTERM (add_signal_handler is not available on Windows,
    # where Ctrl-C falls back to KeyboardInterrupt handled in main()).
    stop = asyncio.get_running_loop().create_future()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, lambda: stop.done() or stop.set_result(None))
        except (NotImplementedError, RuntimeError):
            pass

    async with serve(
        server.handler,
        args.host,
        args.port,
        ssl=ssl_ctx,
        compression=None,       # raw PCM does not compress; the device does not ask for it anyway
        max_size=1 << 20,
    ):
        print_banner(args)
        await stop
        server.shutting_down = True
        print("\nShutting down ...")
    # Leaving the "async with" closes open connections and waits for their handlers,
    # so any session in progress is still saved.


def main():
    ap = argparse.ArgumentParser(description="WebSocket test server for the SIH26172 ESP32-S3 audio stream.")
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default 0.0.0.0 = all)")
    ap.add_argument("--port", type=int, default=8765, help="port (default 8765)")
    ap.add_argument("--out-dir", default=os.path.join(SCRIPT_DIR, "sessions"),
                    help="where to save WAV/JSON files (default: sessions/ next to this script)")
    ap.add_argument("--vosk-model", metavar="PATH", help="run Vosk ASR with this model folder and send transcripts")
    ap.add_argument("--server-eos", action="store_true",
                    help="with --vosk-model: send end_of_speech when Vosk finishes the first utterance")
    ap.add_argument("--server-eos-after-ms", type=int, metavar="N",
                    help="without Vosk: send end_of_speech after N ms of audio (tests the device's server-EOS path)")
    ap.add_argument("--certfile", help="TLS certificate (PEM) to serve wss://")
    ap.add_argument("--keyfile", help="TLS private key (PEM) to serve wss://")
    args = ap.parse_args()

    if args.server_eos and not args.vosk_model:
        ap.error("--server-eos needs --vosk-model (use --server-eos-after-ms without Vosk)")
    if bool(args.certfile) != bool(args.keyfile):
        ap.error("--certfile and --keyfile must be given together")
    os.makedirs(args.out_dir, exist_ok=True)
    args.out_dir = os.path.abspath(args.out_dir)

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except OSError as e:
        sys.exit("Could not start the server: %s" % e)
    print("Bye.")


if __name__ == "__main__":
    main()

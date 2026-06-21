#!/usr/bin/env python3
"""XP Bridge Host Daemon - runs inside the Kasm/QEMU container.
Bridges clipboard, file transfers, and resolution between KasmVNC and XP guest."""

import socket, struct, threading, subprocess, time, os, sys
from pathlib import Path

PORT = 9500
UPLOAD_DIR = Path(os.environ.get("XP_BRIDGE_UPLOAD_DIR", "/home/kasm-user/Uploads"))
DOWNLOAD_DIR = Path(os.environ.get("XP_BRIDGE_DOWNLOAD_DIR", "/home/kasm-user/Downloads"))
MONITOR_SOCK = Path(os.environ.get("XP_BRIDGE_MONITOR", "/tmp/qemu-monitor.sock"))
CHUNK_SIZE = 32768
CLIPBOARD_POLL_INTERVAL = 0.5
HEARTBEAT_INTERVAL = 10
HEARTBEAT_TIMEOUT = 15
RESOLUTION_POLL_INTERVAL = 2

# Message types
MSG_CLIPBOARD_TEXT = 0x01
MSG_FILE_START = 0x02
MSG_FILE_CHUNK = 0x03
MSG_FILE_END = 0x04
MSG_PING = 0x05
MSG_PONG = 0x06
MSG_SET_RESOLUTION = 0x07

# VESA modes supported by -vga std
VESA_MODES = [
    (640, 480), (800, 600), (1024, 768), (1152, 864),
    (1280, 720), (1280, 800), (1280, 1024),
    (1400, 1050), (1440, 900), (1600, 900), (1600, 1200),
    (1680, 1050), (1920, 1080), (1920, 1200), (2560, 1440), (2560, 1600),
]


def log(msg):
    print(f"[xp-bridge] {msg}", flush=True)


def best_vesa_mode(width, height):
    """Find the largest VESA mode that fits within width x height."""
    best = (800, 600)
    for w, h in VESA_MODES:
        if w <= width and h <= height:
            if w * h > best[0] * best[1]:
                best = (w, h)
    return best


def get_x11_resolution():
    """Get current X11 display resolution."""
    try:
        env = os.environ.copy()
        if "DISPLAY" not in env:
            env["DISPLAY"] = ":1"
        r = subprocess.run(["xdpyinfo"], capture_output=True, timeout=2, env=env)
        if r.returncode == 0:
            for line in r.stdout.decode().split("\n"):
                if "dimensions:" in line:
                    parts = line.strip().split()[1]
                    w, h = parts.split("x")
                    return int(w), int(h)
    except (subprocess.TimeoutExpired, FileNotFoundError, ValueError):
        pass
    return None


def send_msg(sock, msg_type, payload=b""):
    header = struct.pack(">IB", len(payload) + 1, msg_type)
    sock.sendall(header + payload)


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Connection closed")
        buf.extend(chunk)
    return bytes(buf)


def recv_msg(sock):
    length_bytes = recv_exact(sock, 4)
    length = struct.unpack(">I", length_bytes)[0]
    if length < 1:
        raise ValueError("Invalid message length")
    data = recv_exact(sock, length)
    return data[0], data[1:]


def get_x11_clipboard():
    try:
        env = os.environ.copy()
        if "DISPLAY" not in env:
            env["DISPLAY"] = ":1"
        r = subprocess.run(["xclip", "-selection", "clipboard", "-o"],
                          capture_output=True, timeout=2, env=env)
        if r.returncode == 0:
            return r.stdout.decode("utf-8", errors="replace")
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    return None


def set_x11_clipboard(text):
    try:
        env = os.environ.copy()
        if "DISPLAY" not in env:
            env["DISPLAY"] = ":1"
        p = subprocess.Popen(["xclip", "-selection", "clipboard"],
                            stdin=subprocess.PIPE, env=env)
        p.communicate(text.encode("utf-8"), timeout=2)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass


def qemu_monitor_cmd(cmd):
    if not MONITOR_SOCK.exists():
        log(f"Monitor socket not available, cannot run: {cmd}")
        return
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(str(MONITOR_SOCK))
        s.recv(4096)  # banner
        s.sendall(f"{cmd}\n".encode())
        time.sleep(0.3)
        resp = s.recv(4096).decode(errors="replace")
        s.close()
        log(f"Monitor: {cmd} -> {resp.strip()}")
    except Exception as e:
        log(f"Monitor error: {e}")


class ClientHandler:
    def __init__(self, conn, addr):
        self.conn = conn
        self.addr = addr
        self.last_clipboard_sent = None
        self.last_clipboard_received = None
        self.running = True
        self.last_recv_time = time.time()
        self.recv_file = None
        self.recv_file_path = None
        self.recv_file_remaining = 0
        self.current_resolution = None

    def run(self):
        log(f"Guest agent connected from {self.addr}")
        recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        recv_thread.start()
        resolution_thread = threading.Thread(target=self._resolution_loop, daemon=True)
        resolution_thread.start()
        self._clipboard_and_heartbeat_loop()

    def _resolution_loop(self):
        """Watch X11 display size and send resolution changes to guest."""
        while self.running:
            res = get_x11_resolution()
            if res and res != self.current_resolution:
                vesa_w, vesa_h = best_vesa_mode(res[0], res[1])
                if (vesa_w, vesa_h) != self.current_resolution:
                    self.current_resolution = (vesa_w, vesa_h)
                    try:
                        self.set_resolution(vesa_w, vesa_h)
                    except Exception:
                        self.running = False
                        break
            time.sleep(RESOLUTION_POLL_INTERVAL)

    def _clipboard_and_heartbeat_loop(self):
        last_heartbeat = time.time()
        while self.running:
            clip = get_x11_clipboard()
            if clip and clip != self.last_clipboard_sent and clip != self.last_clipboard_received:
                self.last_clipboard_sent = clip
                try:
                    send_msg(self.conn, MSG_CLIPBOARD_TEXT, clip.encode("utf-8"))
                except Exception:
                    self.running = False
                    break

            now = time.time()
            if now - last_heartbeat > HEARTBEAT_INTERVAL:
                try:
                    send_msg(self.conn, MSG_PING)
                    last_heartbeat = now
                except Exception:
                    self.running = False
                    break

            if now - self.last_recv_time > HEARTBEAT_TIMEOUT + HEARTBEAT_INTERVAL:
                log("Heartbeat timeout, dropping connection")
                self.running = False
                break

            time.sleep(CLIPBOARD_POLL_INTERVAL)

        self.conn.close()
        log(f"Guest agent disconnected: {self.addr}")

    def _recv_loop(self):
        while self.running:
            try:
                msg_type, payload = recv_msg(self.conn)
                self.last_recv_time = time.time()
            except Exception:
                self.running = False
                break

            if msg_type == MSG_CLIPBOARD_TEXT:
                text = payload.decode("utf-8", errors="replace")
                self.last_clipboard_received = text
                self.last_clipboard_sent = text
                set_x11_clipboard(text)
                log(f"Clipboard from guest: {text[:50]}...")

            elif msg_type == MSG_FILE_START:
                name_len = struct.unpack(">H", payload[:2])[0]
                filename = payload[2:2 + name_len].decode("utf-8")
                file_size = struct.unpack(">Q", payload[2 + name_len:10 + name_len])[0]
                DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
                self.recv_file_path = DOWNLOAD_DIR / filename
                self.recv_file = open(self.recv_file_path, "wb")
                self.recv_file_remaining = file_size
                log(f"Receiving file: {filename} ({file_size} bytes)")

            elif msg_type == MSG_FILE_CHUNK:
                if self.recv_file:
                    self.recv_file.write(payload)
                    self.recv_file_remaining -= len(payload)

            elif msg_type == MSG_FILE_END:
                if self.recv_file:
                    self.recv_file.close()
                    self.recv_file = None
                    log(f"File received: {self.recv_file_path}")

            elif msg_type == MSG_PING:
                try:
                    send_msg(self.conn, MSG_PONG)
                except Exception:
                    self.running = False
                    break

            elif msg_type == MSG_PONG:
                pass

    def send_file(self, filepath):
        filename = filepath.name.encode("utf-8")
        file_size = filepath.stat().st_size
        header = struct.pack(">H", len(filename)) + filename + struct.pack(">Q", file_size)
        send_msg(self.conn, MSG_FILE_START, header)
        with open(filepath, "rb") as f:
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                send_msg(self.conn, MSG_FILE_CHUNK, chunk)
        send_msg(self.conn, MSG_FILE_END)
        log(f"Sent file to guest: {filepath.name} ({file_size} bytes)")

    def set_resolution(self, width, height):
        payload = struct.pack(">II", width, height)
        send_msg(self.conn, MSG_SET_RESOLUTION, payload)
        log(f"Sent resolution change: {width}x{height}")


class UploadWatcher(threading.Thread):
    """Watches upload dir for new files, sends to guest or mounts ISOs."""

    def __init__(self, handler):
        super().__init__(daemon=True)
        self.handler = handler
        self.seen = set()

    def run(self):
        UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
        for f in UPLOAD_DIR.iterdir():
            if f.is_file():
                self.seen.add(f.name)

        while self.handler.running:
            try:
                for f in UPLOAD_DIR.iterdir():
                    if f.is_file() and f.name not in self.seen:
                        self.seen.add(f.name)
                        if f.suffix.lower() == ".iso":
                            log(f"ISO detected: {f.name}, mounting via QEMU monitor")
                            qemu_monitor_cmd(f"change ide1-cd0 {f}")
                        else:
                            self.handler.send_file(f)
            except Exception as e:
                log(f"Upload watcher error: {e}")
            time.sleep(1)


def main():
    UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(1)
    log(f"Listening on port {PORT}")

    while True:
        conn, addr = srv.accept()
        handler = ClientHandler(conn, addr)
        watcher = UploadWatcher(handler)
        watcher.start()
        handler.run()
        log("Waiting for guest to reconnect...")


if __name__ == "__main__":
    main()

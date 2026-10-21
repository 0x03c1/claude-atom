#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Ponte entre os hooks do Claude Code e o AtomS3.

Mantem a porta USB do AtomS3 aberta (reconectando sozinha) e escuta num socket
Unix. Cada hook abre o socket, manda uma linha JSON e, no caso de pedidos de
permissao, espera a resposta do botao.

Mensagens dos hooks:
  {"t":"state", ...}                      repassada ao device
  {"t":"perm","tool":..,"m":..,"ttl":50}  responde {"d":"allow|deny|ask|timeout|offline"}
  {"t":"info"} / {"t":"config",...}       repassadas ao device, devolve a resposta dele
  {"t":"status"}                          responde {"connected":bool,"port":..,"fw":..,"last_state":..}
"""

import fcntl
import json
import os
import queue
import select
import socket
import sys
import threading
import time
import unicodedata
import uuid

import serial
from serial.tools import list_ports

HOME = os.path.expanduser("~/.claude-atom")
SOCK_PATH = os.path.join(HOME, "bridge.sock")
LOCK_PATH = os.path.join(HOME, "bridge.lock")
ESPRESSIF_VID = 0x303A
PING_EVERY = 3.0
IDLE_EXIT_AFTER = 15 * 60  # sai se ficar 15 min sem device


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


PUNCT = str.maketrans({"\u201c": '"', "\u201d": '"', "\u2018": "'", "\u2019": "'", "\u2013": "-",
                       "\u2014": "-", "\u2026": "...", "\u00a0": " "})


def display_text(value, limit=240):
    """A fonte do AtomS3 cobre Latin-1 (acentos do portugues); o resto e removido."""
    text = unicodedata.normalize("NFC", str(value)).translate(PUNCT)
    text = "".join(ch for ch in text if ord(ch) < 256)
    text = " ".join(text.split()) if "\n" not in text else text.strip()
    return text if len(text) <= limit else text[: limit - 3] + "..."


def sanitize(msg):
    return {k: display_text(v) if isinstance(v, str) and k != "t" else v for k, v in msg.items()}


class Device:
    def __init__(self):
        self.ser = None
        self.port = None
        self.fw = None
        self.write_lock = threading.Lock()
        self.decisions = {}  # perm id -> Queue
        self.replies = queue.Queue()  # respostas de info/config
        self.last_state = None
        self.last_seen = time.time()

    @property
    def connected(self):
        return self.ser is not None

    def find_port(self):
        if os.environ.get("CLAUDE_ATOM_PORT"):
            return os.environ["CLAUDE_ATOM_PORT"]
        for p in list_ports.comports():
            if p.vid == ESPRESSIF_VID:
                return p.device
        return None

    def open(self):
        port = self.find_port()
        if not port:
            return False
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.3
        # DTR/RTS em baixo: no USB-Serial/JTAG do ESP32-S3 eles resetam o chip.
        s.dtr = False
        s.rts = False
        s.open()
        self.ser, self.port = s, port
        log("conectado em", port)
        self.send({"t": "ping"})
        if self.last_state:
            self.send(self.last_state)
        return True

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            log("device desconectado")
        self.ser = None
        self.fw = None

    def send(self, msg):
        if not self.ser:
            return False
        data = (json.dumps(sanitize(msg), separators=(",", ":")) + "\n").encode()
        with self.write_lock:
            try:
                self.ser.write(data)
                return True
            except Exception as e:
                log("erro de escrita:", e)
                self.close()
                return False

    def reader_loop(self):
        while True:
            if not self.ser:
                try:
                    if not self.open():
                        time.sleep(1.5)
                        continue
                except Exception as e:
                    log("falha ao abrir porta:", e)
                    time.sleep(1.5)
                    continue
            try:
                line = self.ser.readline()
            except Exception as e:
                log("erro de leitura:", e)
                self.close()
                continue
            if line:
                self.handle(line)

    def handle(self, raw):
        try:
            msg = json.loads(raw.decode(errors="ignore"))
        except ValueError:
            return
        self.last_seen = time.time()
        t = msg.get("t")
        if t in ("hello", "pong"):
            if t == "hello" or self.fw is None:
                log("firmware:", msg.get("fw"))
            self.fw = msg.get("fw")
            if t == "hello" and self.last_state:  # device reiniciou
                self.send(self.last_state)
        elif t in ("info", "config_ok"):
            self.replies.put(msg)
        elif t == "decision":
            q = self.decisions.get(msg.get("id"))
            if q:
                q.put(msg.get("d"))

    def ping_loop(self):
        while True:
            time.sleep(PING_EVERY)
            self.send({"t": "ping"})


class Bridge:
    def __init__(self, device):
        self.dev = device
        self.perm_lock = threading.Lock()  # um pedido de permissao por vez na tela

    def serve(self):
        if os.path.exists(SOCK_PATH):
            os.unlink(SOCK_PATH)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(SOCK_PATH)
        os.chmod(SOCK_PATH, 0o600)
        srv.listen(16)
        log("escutando em", SOCK_PATH)
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=self.client, args=(conn,), daemon=True).start()

    def client(self, conn):
        with conn:
            try:
                f = conn.makefile("rb")
                line = f.readline()
                if not line:
                    return
                msg = json.loads(line)
                reply = self.dispatch(msg, conn)
                if reply is not None:
                    conn.sendall((json.dumps(reply) + "\n").encode())
            except Exception as e:
                log("erro no cliente:", e)

    def dispatch(self, msg, conn):
        t = msg.get("t")
        if t == "state":
            self.dev.last_state = msg
            self.dev.send(msg)
            return None
        if t == "status":
            return {
                "connected": self.dev.connected,
                "port": self.dev.port,
                "fw": self.dev.fw,
                "last_state": self.dev.last_state,
            }
        if t == "perm":
            return self.permission(msg, conn)
        if t in ("info", "config"):
            return self.device_request(msg)
        return {"error": "tipo desconhecido"}

    def device_request(self, msg):
        """Repassa info/config ao device (sem mexer em acentos: sao chaves e senhas)."""
        if not self.dev.connected:
            return {"error": "device desconectado"}
        while not self.dev.replies.empty():
            self.dev.replies.get_nowait()
        data = (json.dumps(msg, separators=(",", ":")) + "\n").encode()
        with self.dev.write_lock:
            self.dev.ser.write(data)
        try:
            return self.dev.replies.get(timeout=5)
        except queue.Empty:
            return {"error": "device nao respondeu"}

    def permission(self, msg, conn):
        ttl = int(msg.get("ttl", 50))
        deadline = time.time() + ttl
        # Outros pedidos esperam a vez (sem estourar o prazo do hook).
        while not self.perm_lock.acquire(timeout=0.5):
            if time.time() > deadline or client_gone(conn):
                return {"d": "timeout"}
        try:
            if not self.dev.connected:
                return {"d": "offline"}
            pid = uuid.uuid4().hex[:8]
            q = queue.Queue()
            self.dev.decisions[pid] = q
            remaining = max(1, int(deadline - time.time()))
            self.dev.send({**msg, "id": pid, "ttl": remaining})
            log(f"permissao {pid}: {msg.get('tool')} {msg.get('m', '')[:60]!r}")
            try:
                while True:
                    try:
                        d = q.get(timeout=0.5)
                        log(f"permissao {pid}: {d}")
                        return {"d": d}
                    except queue.Empty:
                        pass
                    if time.time() > deadline or client_gone(conn) or not self.dev.connected:
                        self.dev.send({"t": "cancel", "id": pid})
                        log(f"permissao {pid}: cancelada")
                        return {"d": "timeout"}
            finally:
                self.dev.decisions.pop(pid, None)
        finally:
            self.perm_lock.release()


def client_gone(conn):
    """True se o hook do outro lado fechou o socket (ex.: respondido no terminal)."""
    r, _, _ = select.select([conn], [], [], 0)
    if not r:
        return False
    try:
        return conn.recv(1, socket.MSG_PEEK) == b""
    except OSError:
        return True


def watchdog(device):
    while True:
        time.sleep(30)
        if not device.connected and time.time() - device.last_seen > IDLE_EXIT_AFTER:
            log("sem device ha muito tempo, encerrando")
            os._exit(0)


def main():
    os.makedirs(HOME, exist_ok=True)
    lock = open(LOCK_PATH, "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("bridge ja esta rodando", file=sys.stderr)
        sys.exit(0)

    device = Device()
    for target in (device.reader_loop, device.ping_loop, lambda: watchdog(device)):
        threading.Thread(target=target, daemon=True).start()
    Bridge(device).serve()


if __name__ == "__main__":
    main()

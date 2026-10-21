#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "aiohttp>=3.9",
#   "numpy>=1.26",
#   "opuslib>=3.0.1",
#   "webrtcvad-wheels>=2.0.14",
#   "faster-whisper>=1.1",
#   "piper-tts>=1.3",
#   "anthropic>=1.17",
# ]
# ///
"""Servidor xiaozhi com o Claude como cerebro (API da Anthropic, Claude Code ou OpenCode).

Substitui a nuvem da Tenclass (api.tenclass.net) para o firmware xiaozhi do AtomS3 + Echo Base.
O aparelho faz a palavra de ativacao ("Hi, M Five") e o audio Opus; aqui roda:

  voz (Opus 16 kHz) -> VAD (webrtcvad) -> Whisper local (faster-whisper)
       -> API da Anthropic em streaming (padrao), Claude Code headless (`claude -p`) ou `opencode run`
       -> voz Piper (ou `say` no macOS) -> Opus 24 kHz -> alto-falante

A API so troca texto: a voz entra e sai por aqui, no Mac.

Rotas (mesma porta):
  POST /xiaozhi/ota/<segredo>/   o aparelho pede a configuracao; respondemos com o nosso WebSocket
  GET  /xiaozhi/v1/              WebSocket do protocolo xiaozhi (exige o token entregue no OTA)

Configuracao por argumentos ou variaveis de ambiente:
  ANTHROPIC_API_KEY    chave da API (agente "api")
  CLAUDE_ATOM_SECRET   segredo do caminho de OTA e do token (sem ele, OTA aberto em /xiaozhi/ota/)
  ALLOWED_DEVICES      MACs autorizados, separados por virgula (opcional)
  PUBLIC_URL           endereco publico (ex.: Tailscale Funnel) usado para montar o WebSocket do OTA
  DATA_DIR             onde guardar modelos, vozes e a pasta de trabalho (padrao ~/.claude-atom)
"""

from __future__ import annotations

import argparse
import asyncio
import ctypes.util
import hashlib
import hmac
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid
import wave
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from aiohttp import WSMsgType, web

# O opuslib procura a libopus via ctypes; no macOS o Homebrew fica fora do caminho padrao.
_find_library = ctypes.util.find_library


def _find_library_with_brew(name):
    found = _find_library(name)
    if found or name != "opus":
        return found
    for path in ("/opt/homebrew/lib/libopus.dylib", "/usr/local/lib/libopus.dylib"):
        if os.path.exists(path):
            return path
    return None


ctypes.util.find_library = _find_library_with_brew
import opuslib  # noqa: E402
import webrtcvad  # noqa: E402

log = logging.getLogger("claude-xiaozhi")

IN_RATE = 16000  # o aparelho envia 16 kHz
OUT_RATE = 24000  # e aceita 24 kHz na volta
FRAME_MS = 60
VAD_FRAME = IN_RATE * 30 // 1000  # webrtcvad aceita 10/20/30 ms
PIPER_VOICES = "https://huggingface.co/rhasspy/piper-voices/resolve/main"

VOICE_PROMPT = (
    "Voce e o Claude conversando por VOZ atraves de um pequeno aparelho (M5Stack AtomS3 com alto-falante). "
    "Tudo o que voce escrever sera lido em voz alta e mostrado numa tela minuscula. Responda em texto "
    "corrido, sem markdown, listas, titulos, tabelas, emojis ou blocos de codigo; nunca leia codigo em voz "
    "alta, descreva-o. Seja breve: uma a tres frases, a menos que peçam detalhes. Responda no idioma em que "
    "falarem com voce (padrao: portugues do Brasil). A pergunta vem de reconhecimento de voz e pode ter erros "
    "de transcricao; interprete com bom senso. Se precisar usar ferramentas, use, mas resuma o resultado."
)


@dataclass
class Config:
    host: str = "0.0.0.0"
    port: int = 8003
    agent: str = "api"  # api | claude | opencode
    data_dir: Path = Path.home() / ".claude-atom"
    workdir: Path = Path.home() / "ClaudeAtom"
    model: str | None = None
    effort: str = "low"  # agente api: low responde mais rapido; medium/high pensam mais
    public_url: str = ""
    tools: str = "Read,Grep,Glob,WebSearch,WebFetch"
    tts: str = "piper"  # piper | say
    voice: str = "pt_BR-faber-medium"
    rate: float = 1.0
    whisper: str = "small"
    language: str | None = "pt"
    silence_ms: int = 800
    idle_close_s: int = 30
    session_ttl_s: int = 15 * 60
    secret: str = ""
    insecure: bool = False  # sem segredo: so para a rede local
    log_text: bool = False  # escrever no log o que foi dito (privacidade)
    budget_usd: float = 5.0  # aviso por voz a cada multiplo deste gasto estimado (0 desliga)
    budget_period: str = "day"  # day | month
    allowed: frozenset = frozenset()

    @property
    def token(self) -> str:
        # token do WebSocket derivado do segredo (o segredo em si so aparece na URL de OTA)
        return hmac.new(self.secret.encode(), b"xiaozhi-ws", hashlib.sha256).hexdigest()[:32] if self.secret else ""


# ---------------------------------------------------------------- ASR / TTS


class Transcriber:
    """Whisper local (CPU int8). Carrega em segundo plano para nao atrasar o servidor."""

    def __init__(self, size: str, language: str | None):
        self.size = size
        self.language = language
        self.model = None
        self.ready = asyncio.Event()

    def load(self):
        from faster_whisper import WhisperModel

        threads = cpu_limit()
        log.info("carregando Whisper '%s' com %d thread(s) (a primeira vez baixa o modelo)...", self.size, threads)
        self.model = WhisperModel(self.size, device="cpu", compute_type="int8", cpu_threads=threads)
        log.info("Whisper pronto")

    def transcribe(self, pcm: np.ndarray) -> str:
        audio = pcm.astype(np.float32) / 32768.0
        segments, _ = self.model.transcribe(
            audio, language=self.language, beam_size=1, vad_filter=False, condition_on_previous_text=False,
            initial_prompt="Conversa com o Claude.",  # sem a dica, "Claude" vira "Club", "cloud"
        )
        return " ".join(s.text.strip() for s in segments).strip()


def clean(value: str) -> str:
    """Cabecalho vindo de fora, pronto para log e para chave de dicionario."""
    return re.sub(r"[^0-9A-Za-z:._-]", "?", value)[:40]


def said(cfg: "Config", text: str) -> str:
    return text if cfg.log_text else f"({len(text)} caracteres)"


def cpu_limit() -> int:
    """CPUs que o processo pode usar de fato. Num container (Render) os.cpu_count() mostra a maquina
    inteira; com mais threads do que a cota, o Whisper fica muito mais lento."""
    if threads := os.environ.get("ASR_THREADS"):
        return max(1, int(threads))
    try:
        quota, period = Path("/sys/fs/cgroup/cpu.max").read_text().split()  # cgroup v2
        if quota != "max":
            return max(1, round(int(quota) / int(period)))
    except (OSError, ValueError):
        pass
    try:
        return max(1, len(os.sched_getaffinity(0)))
    except AttributeError:  # macOS
        return min(4, os.cpu_count() or 1)


def resample(pcm: np.ndarray, src: int, dst: int) -> np.ndarray:
    if src == dst or not len(pcm):
        return pcm
    n = int(len(pcm) * dst / src)
    x = np.linspace(0, len(pcm) - 1, n)
    return np.interp(x, np.arange(len(pcm)), pcm.astype(np.float32)).astype(np.int16)


class Voice:
    """Texto -> PCM int16 mono 24 kHz, com Piper (Linux/macOS) ou `say` (macOS)."""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.piper = None

    def load(self):
        if self.cfg.tts != "piper":
            return
        from piper import PiperVoice

        path = self.cfg.data_dir / "voices" / f"{self.cfg.voice}.onnx"
        if not path.exists():
            lang, rest = self.cfg.voice.split("-", 1)
            name, quality = rest.rsplit("-", 1)
            base = f"{PIPER_VOICES}/{lang.split('_')[0]}/{lang}/{name}/{quality}/{self.cfg.voice}"
            path.parent.mkdir(parents=True, exist_ok=True)
            log.info("baixando a voz %s...", self.cfg.voice)
            for ext in (".onnx.json", ".onnx"):
                tmp = path.with_name(path.name.replace(".onnx", ext) + ".part")
                urllib.request.urlretrieve(base + ext, tmp)
                tmp.rename(path.with_name(path.name.replace(".onnx", ext)))
        self.piper = PiperVoice.load(str(path))
        log.info("voz %s pronta (%d Hz)", self.cfg.voice, self.piper.config.sample_rate)

    def synthesize(self, text: str) -> np.ndarray:
        if self.piper:
            from piper import SynthesisConfig

            chunks = list(self.piper.synthesize(text, SynthesisConfig(length_scale=1 / self.cfg.rate)))
            pcm = np.concatenate([c.audio_int16_array for c in chunks]) if chunks else np.zeros(0, np.int16)
            return resample(pcm, self.piper.config.sample_rate, OUT_RATE)
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "tts.wav")
            subprocess.run(
                ["say", "-v", self.cfg.voice, "-r", str(int(190 * self.cfg.rate)), "-o", out,
                 "--file-format=WAVE", f"--data-format=LEI16@{OUT_RATE}", "-f", "-"],
                input=text.encode(), check=True, capture_output=True,
            )
            with wave.open(out, "rb") as w:
                return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)


MD_PATTERNS = [
    (re.compile(r"```.*?```", re.S), " (codigo omitido) "),
    (re.compile(r"`([^`]*)`"), r"\1"),
    (re.compile(r"\[([^\]]+)\]\([^)]+\)"), r"\1"),
    (re.compile(r"^\s*(#+|[-*+]|\d+\.)\s+", re.M), ""),
    (re.compile(r"[*_#>|]+"), ""),
]
SENTENCE_END = re.compile(r"(?<=[.!?…:;])\s+|\n+")


def speakable(text: str) -> str:
    for pattern, repl in MD_PATTERNS:
        text = pattern.sub(repl, text)
    return " ".join(text.split())


class SentenceSplitter:
    """Junta o texto em streaming e solta frases completas para a voz comecar cedo."""

    def __init__(self, min_len: int = 12):
        self.buf = ""
        self.min_len = min_len

    def feed(self, chunk: str) -> list[str]:
        self.buf += chunk
        if self.buf.count("```") % 2 == 1:  # espera fechar o bloco de codigo
            return []
        out = []
        while True:
            m = SENTENCE_END.search(self.buf)
            if not m:
                break
            head, self.buf = self.buf[: m.end()], self.buf[m.end():]
            if len(head.strip()) < self.min_len and self.buf:
                self.buf = head + self.buf
                nxt = SENTENCE_END.search(self.buf, m.end())
                if not nxt:
                    break
                head, self.buf = self.buf[: nxt.end()], self.buf[nxt.end():]
            s = speakable(head)
            if s:
                out.append(s)
        return out

    def flush(self) -> list[str]:
        s = speakable(self.buf)
        self.buf = ""
        return [s] if s else []


# ---------------------------------------------------------------- agentes


class Agent:
    """Mantem uma sessao do agente por aparelho, renovada apos `ttl` sem uso."""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.sessions: dict[str, tuple[str, float]] = {}

    def session_for(self, device: str) -> str | None:
        sid, last = self.sessions.get(device, (None, 0))
        return sid if sid and time.time() - last < self.cfg.session_ttl_s else None

    def remember(self, device: str, sid: str | None):
        if sid:
            self.sessions[device] = (sid, time.time())

    def forget(self, device: str):
        self.sessions.pop(device, None)

    async def _lines(self, args: list[str]):
        proc = await asyncio.create_subprocess_exec(
            *args, cwd=self.cfg.workdir, stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, limit=8 * 1024 * 1024,
        )
        try:
            async for raw in proc.stdout:
                line = raw.decode(errors="ignore").strip()
                if line.startswith("{"):
                    try:
                        yield json.loads(line)
                    except ValueError:
                        pass
            await proc.wait()
            if proc.returncode:
                err = (await proc.stderr.read()).decode(errors="ignore").strip()
                log.warning("agente saiu com %s: %s", proc.returncode, err[-500:])
        finally:
            if proc.returncode is None:
                proc.kill()
                await proc.wait()


class ClaudeCodeAgent(Agent):
    async def stream(self, device: str, text: str, device_tools=None):
        claude = shutil.which("claude") or str(Path.home() / ".local/bin/claude")
        args = [claude, "-p", text, "--output-format", "stream-json", "--include-partial-messages",
                "--verbose", "--append-system-prompt", VOICE_PROMPT, "--allowedTools", self.cfg.tools]
        if self.cfg.model:
            args += ["--model", self.cfg.model]
        if sid := self.session_for(device):
            args += ["--resume", sid]
        async for ev in self._lines(args):
            t = ev.get("type")
            if t == "stream_event":
                e = ev.get("event", {})
                if e.get("type") == "content_block_delta" and e.get("delta", {}).get("type") == "text_delta":
                    yield e["delta"]["text"]
                elif e.get("type") == "content_block_start" and e.get("content_block", {}).get("type") == "tool_use":
                    yield "\n"  # fecha a frase antes de a ferramenta rodar
            elif t in ("system", "result") and ev.get("session_id"):
                self.remember(device, ev["session_id"])
                if t == "result" and ev.get("is_error"):
                    log.warning("Claude Code: %s", ev.get("result"))
                    yield "\nDesculpe, deu um erro no Claude Code."


API_MODELS = {"opus": "claude-opus-5-5", "sonnet": "claude-sonnet-5-5", "haiku": "claude-haiku-5-5"}

# US$ por milhao de tokens: entrada, saida, leitura de cache; a escrita de cache (5 min) custa 1,25x a entrada.
# Estimativa para o aviso de gasto: o valor exato esta no Console da Anthropic.
PRICES = {"claude-opus-5-5": (4.0, 20.0, 0.20), "claude-sonnet-5-5": (2.0, 10.0, 0.20),
          "claude-haiku-5-5": (0.10, 0.50, 0.01)}
WEB_SEARCH_USD = 10.0 / 1000


def dollars(value: float) -> str:
    """Valor por extenso para a voz: "5 dólares e 12 centavos" (ela le mal "5.12")."""
    whole, cents = int(value), round((value - int(value)) * 100)
    if cents == 100:
        whole, cents = whole + 1, 0
    if not whole:
        return f"{cents} centavos"
    text = f"{whole} dólar" + ("" if whole == 1 else "es")
    return text + (f" e {cents} centavos" if cents else "")


class Budget:
    """Soma o custo estimado das respostas no periodo (dia ou mes, horario de Sao Paulo) e avisa a cada
    multiplo do limite. Fica na memoria: um deploy zera a conta."""

    def __init__(self, limit_usd: float, period: str):
        self.limit, self.period = limit_usd, period
        self.key, self.spent, self.warned = "", 0.0, 0

    def add(self, model: str, usage) -> float:
        key = time.strftime("%Y-%m-%d" if self.period == "day" else "%Y-%m")
        if key != self.key:
            self.key, self.spent, self.warned = key, 0.0, 0
        price_in, price_out, price_read = PRICES.get(model, PRICES["claude-opus-5-5"])  # desconhecido: o mais caro
        tool_use = getattr(usage, "server_tool_use", None)
        cost = ((usage.input_tokens or 0) * price_in + (usage.output_tokens or 0) * price_out
                + (getattr(usage, "cache_read_input_tokens", 0) or 0) * price_read
                + (getattr(usage, "cache_creation_input_tokens", 0) or 0) * price_in * 1.25) / 1e6
        cost += (getattr(tool_use, "web_search_requests", 0) or 0) * WEB_SEARCH_USD
        self.spent += cost
        return cost

    def warning(self) -> str | None:
        """Frase de aviso quando o gasto passa de mais um multiplo do limite; senao None."""
        if self.limit <= 0 or self.spent < (self.warned + 1) * self.limit:
            return None
        self.warned = int(self.spent // self.limit)
        when = "hoje" if self.period == "day" else "este mês"
        return (f"Atenção: esta atividade está consumindo muitos tokens. {when.capitalize()} já foram cerca de "
                f"{dollars(self.spent)}, e o limite de aviso é de {dollars(self.limit)}.")

# Ferramentas do aparelho que o Claude pode usar. Lista fechada de proposito: o firmware tambem oferece
# coisas como gravar firmware de uma URL, e um texto da busca na web nao pode chegar ate elas.
DEVICE_TOOLS = ("self.get_device_status", "self.audio_speaker.set_volume", "self.screen.set_brightness",
                "self.screen.set_theme")


class DeviceTools:
    """Ferramentas MCP do proprio aparelho (estado, volume, tela), pelo canal "mcp" do protocolo xiaozhi."""

    def __init__(self, send):
        self.send = send  # coroutine que recebe o payload JSON-RPC
        self.tools: list[dict] = []  # no formato da API
        self.names: dict[str, str] = {}  # nome na API -> nome no aparelho
        self.pending: dict[int, asyncio.Future] = {}
        self.last_id = 0

    async def request(self, method: str, params: dict, timeout: float = 5.0) -> dict:
        self.last_id += 1
        rid = self.last_id
        self.pending[rid] = asyncio.get_running_loop().create_future()
        try:
            await self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
            return await asyncio.wait_for(self.pending[rid], timeout)
        finally:
            self.pending.pop(rid, None)

    def on_message(self, payload: dict):
        fut = self.pending.get(payload.get("id"))
        if fut and not fut.done():
            fut.set_result(payload)

    async def discover(self):
        await self.request("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                          "clientInfo": {"name": "claude-atom", "version": "1"}})
        cursor = ""
        for _ in range(5):  # paginas de tools/list
            result = (await self.request("tools/list", {"cursor": cursor})).get("result") or {}
            for tool in result.get("tools", []):
                name = tool.get("name", "")
                if name not in DEVICE_TOOLS:
                    continue
                api_name = re.sub(r"[^a-zA-Z0-9_-]", "_", name)  # a API nao aceita "." no nome
                self.names[api_name] = name
                self.tools.append({"name": api_name, "description": tool.get("description", "")[:1000],
                                   "input_schema": tool.get("inputSchema") or {"type": "object", "properties": {}},
                                   "eager_input_streaming": True})
            cursor = result.get("nextCursor") or ""
            if not cursor:
                break

    async def call(self, api_name: str, arguments) -> tuple[str, bool]:
        """Devolve (texto, erro?). O firmware confere os tipos e limites dos argumentos."""
        name = self.names.get(api_name)
        if not name or not isinstance(arguments, dict):
            return "ferramenta ou argumentos invalidos", True
        try:
            reply = await self.request("tools/call", {"name": name, "arguments": arguments})
        except asyncio.TimeoutError:
            return "o aparelho nao respondeu", True
        if "error" in reply:
            return str((reply["error"] or {}).get("message", "erro")), True
        result = reply.get("result") or {}
        text = " ".join(c.get("text", "") for c in result.get("content", []) if isinstance(c, dict))
        return text or "ok", bool(result.get("isError"))


def echoable(content: list) -> list:
    """Blocos da resposta para devolver no historico. Depois de um fallback no meio da resposta, a API
    pede que se omitam os blocos internos (raciocinio, ferramenta sem resultado) anteriores ao ultimo
    bloco "fallback"."""
    cut = max((i for i, b in enumerate(content) if b.type == "fallback"), default=-1)
    if cut < 0:
        return content
    head = content[:cut]
    answered = {b.tool_use_id for b in head if b.type.endswith("_tool_result")}
    keep = [b for b in head if b.type == "text" or b.type.endswith("_tool_result")
            or (b.type == "server_tool_use" and b.id in answered)]
    return keep + content[cut:]


class ApiAgent(Agent):
    """API da Anthropic em streaming. O historico de cada aparelho fica na memoria do servidor."""

    MAX_TURNS = 40  # passou disso, conversa nova (cortar o inicio invalidaria o raciocinio guardado)

    def __init__(self, cfg: Config):
        super().__init__(cfg)
        import anthropic

        self.anthropic = anthropic
        self.client = anthropic.AsyncAnthropic()
        self.model = API_MODELS.get(cfg.model or "opus", cfg.model)
        self.histories: dict[str, list] = {}
        haiku = "haiku" in self.model
        self.tools = [{
            "type": "web_search_20250305" if haiku else "web_search_20260209", "name": "web_search", "max_uses": 3,
            "user_location": {"type": "approximate", "country": "BR", "timezone": "America/Sao_Paulo"},
        }]
        # se o filtro de seguranca recusar, a API refaz a pergunta noutro modelo (o Haiku nao tem fallback)
        self.extra = {} if haiku else {"betas": ["server-side-fallback-2026-07-01"], "fallbacks": "default"}
        self.budget = Budget(cfg.budget_usd, cfg.budget_period)

    def forget(self, device: str):
        super().forget(device)
        self.histories.pop(device, None)

    async def stream(self, device: str, text: str, device_tools: DeviceTools | None = None):
        history = self.histories.get(device, []) if self.session_for(device) else []
        if len(history) >= 2 * self.MAX_TURNS:
            history = []
        # a data vai na pergunta, nao no prompt de sistema, para nao quebrar o cache
        messages = history + [{"role": "user", "content": f"[{time.strftime('%d/%m/%Y %H:%M')}] {text}"}]
        tools = self.tools + (device_tools.tools if device_tools else [])
        searching = False
        try:
            # varias voltas: pause_turn da busca na web e chamadas das ferramentas do aparelho
            for _ in range(8):
                async with self.client.beta.messages.stream(
                    model=self.model, max_tokens=8000, system=VOICE_PROMPT, messages=messages, tools=tools,
                    output_config={"effort": self.cfg.effort}, cache_control={"type": "ephemeral"}, **self.extra,
                ) as stream:
                    async for event in stream:
                        if event.type == "text":
                            yield event.text
                        elif (event.type == "content_block_start" and event.content_block.type == "server_tool_use"
                              and not searching):
                            searching = True  # avisa uma vez, para nao ficar em silencio durante a busca
                            yield "\nSó um instante, vou pesquisar.\n"
                    final = await stream.get_final_message()
                cost = self.budget.add(final.model, final.usage)
                log.info("[%s] custo ~US$ %.4f (%s: US$ %.2f de %.2f)", device, cost, self.budget.key,
                         self.budget.spent, self.budget.limit)
                if final.stop_reason == "refusal":
                    log.warning("API recusou (%s): %s", final.stop_details, said(self.cfg, text))
                    yield "\nDesculpe, não posso ajudar com isso."
                    return  # a pergunta recusada nao entra no historico
                if final.stop_reason == "max_tokens":
                    log.warning("resposta cortada em max_tokens")
                    return  # uma chamada de ferramenta pela metade quebraria o historico
                messages.append({"role": "assistant", "content": echoable(final.content)})
                if final.stop_reason == "pause_turn":
                    continue
                uses = [b for b in final.content if b.type == "tool_use"]
                if final.stop_reason != "tool_use" or not uses or not device_tools:
                    break
                results = []
                for use in uses:
                    output, is_error = await device_tools.call(use.name, use.input)
                    log.info("[%s] ferramenta %s %s -> %s", device, use.name, use.input, output[:200])
                    results.append({"type": "tool_result", "tool_use_id": use.id, "content": output,
                                    "is_error": is_error})
                messages.append({"role": "user", "content": results})
        except ValueError:  # argumentos de ferramenta em JSON invalido (streaming antecipado)
            log.warning("argumentos invalidos numa chamada de ferramenta")
            yield "\nDesculpe, me enrolei com o aparelho. Pode repetir?"
            return
        except self.anthropic.AuthenticationError:
            log.error("ANTHROPIC_API_KEY ausente ou invalida")
            yield "\nA chave da API da Anthropic não está configurada."
            return
        except (self.anthropic.RateLimitError, self.anthropic.APIConnectionError) as e:
            log.warning("API indisponivel: %s", e)
            yield "\nNão consegui falar com o Claude agora. Tente de novo daqui a pouco."
            return
        except self.anthropic.APIStatusError as e:
            log.warning("API: %s %s", e.status_code, e.message)
            yield "\nDesculpe, deu um erro na API do Claude."
            return
        log.info("[%s] tokens: %s entrada (%s do cache), %s saida", device, final.usage.input_tokens,
                 final.usage.cache_read_input_tokens, final.usage.output_tokens)
        self.histories[device] = messages
        self.remember(device, "api")
        if warning := self.budget.warning():
            log.warning("[%s] gasto passou do limite: US$ %.2f", device, self.budget.spent)
            yield "\n" + warning


class OpenCodeAgent(Agent):
    async def stream(self, device: str, text: str, device_tools=None):
        opencode = shutil.which("opencode") or str(Path.home() / ".opencode/bin/opencode")
        sid = self.session_for(device)
        message = text if sid else f"[Instrucoes de estilo: {VOICE_PROMPT}]\n\n{text}"
        args = [opencode, "run", "--format", "json"]
        if self.cfg.model:
            args += ["--model", self.cfg.model]
        if sid:
            args += ["--session", sid]
        args.append(message)
        async for ev in self._lines(args):
            self.remember(device, ev.get("sessionID"))
            part = ev.get("part") or {}
            if ev.get("type") == "text" and part.get("text"):
                yield part["text"] + "\n"


# ---------------------------------------------------------------- sessao WebSocket


@dataclass
class Session:
    ws: web.WebSocketResponse
    device: str
    server: "Server"
    session_id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    decoder: opuslib.Decoder = field(default_factory=lambda: opuslib.Decoder(IN_RATE, 1))
    encoder: opuslib.Encoder = field(default_factory=lambda: opuslib.Encoder(OUT_RATE, 1, opuslib.APPLICATION_AUDIO))
    vad: webrtcvad.Vad = field(default_factory=lambda: webrtcvad.Vad(2))
    listening: bool = False
    mode: str = "auto"
    speech: list = field(default_factory=list)
    pending: np.ndarray = field(default_factory=lambda: np.zeros(0, np.int16))
    voiced_run: int = 0
    silence_ms: int = 0
    in_speech: bool = False
    last_activity: float = field(default_factory=time.time)
    task: asyncio.Task | None = None
    tools: DeviceTools | None = None

    async def send(self, **msg):
        msg.setdefault("session_id", self.session_id)
        if not self.ws.closed:
            await self.ws.send_str(json.dumps(msg, ensure_ascii=False))

    # -- entrada

    async def on_json(self, msg: dict):
        t = msg.get("type")
        if t == "hello":
            await self.send(type="hello", transport="websocket", audio_params={
                "format": "opus", "sample_rate": OUT_RATE, "channels": 1, "frame_duration": FRAME_MS})
            if (msg.get("features") or {}).get("mcp") and not self.tools:
                self.tools = DeviceTools(lambda payload: self.send(type="mcp", payload=payload))
                asyncio.create_task(self.discover_tools())
        elif t == "listen":
            state = msg.get("state")
            if state == "start":
                self.mode = msg.get("mode", "auto")
                self.start_listening()
            elif state == "stop":
                self.listening = False
                if self.speech:
                    self.respond(self.take_utterance())
            elif state == "detect":
                log.info("[%s] palavra de ativacao: %s", self.device, msg.get("text"))
        elif t == "abort":
            log.info("[%s] interrompido pelo aparelho", self.device)
            await self.cancel()
            await self.send(type="tts", state="stop")
        elif t == "mcp" and self.tools and isinstance(msg.get("payload"), dict):
            self.tools.on_message(msg["payload"])
        # outros tipos: ignorados

    async def discover_tools(self):
        try:
            await self.tools.discover()
            log.info("[%s] ferramentas do aparelho: %s", self.device, ", ".join(self.tools.names.values()) or "nenhuma")
        except Exception as e:  # sem ferramentas a conversa continua normal
            log.warning("[%s] nao consegui listar as ferramentas do aparelho: %r", self.device, e)

    def busy(self) -> bool:
        return bool(self.task and not self.task.done())

    def start_listening(self):
        self.listening = True
        self.speech, self.voiced_run, self.silence_ms, self.in_speech = [], 0, 0, False
        self.pending = np.zeros(0, np.int16)
        self.last_activity = time.time()

    def on_audio(self, packet: bytes):
        if not self.listening or self.busy():  # enquanto responde, o microfone e ignorado
            return
        try:
            pcm = np.frombuffer(self.decoder.decode(packet, IN_RATE * FRAME_MS // 1000), dtype=np.int16)
        except opuslib.OpusError:  # pacote corrompido: descarta e segue a conversa
            log.debug("[%s] pacote Opus invalido (%d bytes)", self.device, len(packet))
            return
        self.pending = np.concatenate([self.pending, pcm])
        while len(self.pending) >= VAD_FRAME:
            frame, self.pending = self.pending[:VAD_FRAME], self.pending[VAD_FRAME:]
            voiced = self.vad.is_speech(frame.tobytes(), IN_RATE)
            if self.in_speech:
                self.speech.append(frame)
                self.silence_ms = 0 if voiced else self.silence_ms + 30
                long_enough = len(self.speech) * 30 > 30000
                if long_enough or (self.mode == "auto" and self.silence_ms >= self.server.cfg.silence_ms):
                    self.listening = False
                    self.respond(self.take_utterance())
                    return
            else:
                self.voiced_run = self.voiced_run + 1 if voiced else 0
                self.speech = (self.speech + [frame])[-10:]  # 300 ms antes da fala
                if self.voiced_run >= 3:
                    self.in_speech = True
                    self.last_activity = time.time()

    def take_utterance(self) -> np.ndarray:
        audio = np.concatenate(self.speech) if self.speech else np.zeros(0, np.int16)
        self.speech, self.in_speech = [], False
        return audio

    # -- resposta

    def respond(self, audio: np.ndarray):
        self.task = asyncio.create_task(self.answer(audio))

    async def cancel(self):
        if self.task and not self.task.done():
            self.task.cancel()
            try:
                await self.task
            except asyncio.CancelledError:
                pass

    async def answer(self, audio: np.ndarray):
        srv = self.server
        try:
            if len(audio) < IN_RATE * 0.4:
                return self.start_listening()
            await srv.ready.wait()
            t0 = time.time()
            text = await asyncio.to_thread(srv.asr.transcribe, audio)
            log.info("[%s] voce (%.1fs): %s", self.device, time.time() - t0, said(self.server.cfg, text))
            if len(text) < 2:
                return self.start_listening()
            await self.send(type="stt", text=text)
            await self.send(type="llm", emotion="thinking", text="🤔")
            if re.search(r"\b(nova conversa|esquece tudo|reinicia a conversa)\b", text, re.I):
                srv.agent.forget(self.device)
                return await self.speak_all(["Certo, começando uma conversa nova."])
            tools = self.tools if self.tools and self.tools.tools else None
            await self.speak_stream(srv.agent.stream(self.device, text, tools))
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("erro respondendo")
            await self.speak_all(["Desculpe, algo deu errado no servidor."])

    async def speak_all(self, sentences: list[str]):
        async def gen():
            for s in sentences:
                yield s + "\n"

        await self.speak_stream(gen())

    async def speak_stream(self, chunks):
        """Produtor: texto do agente -> frases. Consumidor: frase -> voz -> Opus no ritmo certo."""
        voice = self.server.voice
        queue: asyncio.Queue = asyncio.Queue()

        async def produce():
            splitter = SentenceSplitter()
            try:
                async for chunk in chunks:
                    for s in splitter.feed(chunk):
                        await queue.put(asyncio.create_task(asyncio.to_thread(voice.synthesize, s)))
                        await queue.put(s)
                for s in splitter.flush():
                    await queue.put(asyncio.create_task(asyncio.to_thread(voice.synthesize, s)))
                    await queue.put(s)
            finally:
                await queue.put(None)

        producer = asyncio.create_task(produce())
        started = False
        play_start = sent_ms = 0.0
        frame = OUT_RATE * FRAME_MS // 1000
        try:
            while (synth := await queue.get()) is not None:
                sentence = await queue.get()
                pcm = await synth
                if not started:
                    await self.send(type="tts", state="start")
                    await self.send(type="llm", emotion="neutral", text="🙂")
                    started = True
                    play_start = time.monotonic()
                log.info("[%s] claude: %s", self.device, said(self.server.cfg, sentence))
                await self.send(type="tts", state="sentence_start", text=sentence)
                pad = (-len(pcm)) % frame
                pcm = np.concatenate([pcm, np.zeros(pad + frame, np.int16)])  # + 60 ms de respiro
                elapsed = (time.monotonic() - play_start) * 1000
                if sent_ms < elapsed:  # houve espera pela proxima frase: realinha o relogio
                    play_start = time.monotonic() - sent_ms / 1000
                for i in range(0, len(pcm), frame):
                    # a fila do aparelho guarda 2,4 s; ficamos no maximo ~0,8 s a frente
                    while sent_ms - (time.monotonic() - play_start) * 1000 > 800:
                        await asyncio.sleep(0.02)
                    await self.ws.send_bytes(self.encoder.encode(pcm[i:i + frame].tobytes(), frame))
                    sent_ms += FRAME_MS
            # espera o aparelho terminar de tocar antes do "stop"
            remaining = sent_ms / 1000 - (time.monotonic() - play_start)
            if remaining > 0:
                await asyncio.sleep(remaining)
        finally:
            producer.cancel()
            if started:
                await self.send(type="tts", state="stop")
            else:
                self.start_listening()
            self.last_activity = time.time()


# ---------------------------------------------------------------- servidor HTTP/WebSocket


class Server:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.asr = Transcriber(cfg.whisper, cfg.language)
        self.voice = Voice(cfg)
        self.agent: Agent = {"api": ApiAgent, "claude": ClaudeCodeAgent, "opencode": OpenCodeAgent}[cfg.agent](cfg)
        self.ready = asyncio.Event()
        self.sessions: dict[str, Session] = {}
        self.last_warning = 0.0

    MAX_SESSIONS = 4

    def warn(self, msg: str, *args):
        """Avisos de acesso negado, no maximo um a cada 10 s (nao deixa ninguem inundar o log)."""
        if time.time() - self.last_warning > 10:
            self.last_warning = time.time()
            log.warning(msg, *args)

    def device_allowed(self, request: web.Request) -> bool:
        device = clean(request.headers.get("Device-Id", "")).lower()
        if self.cfg.allowed and device not in self.cfg.allowed:
            self.warn("aparelho nao autorizado: %s (%s)", device or "?", request.remote)
            return False
        return True

    async def ota(self, request: web.Request):
        if not hmac.compare_digest(request.match_info.get("secret", ""), self.cfg.secret) \
                or not self.device_allowed(request):
            raise web.HTTPNotFound()
        try:
            body = await request.json() if request.can_read_body else {}
        except ValueError:
            body = {}
        version = (body.get("application") or {}).get("version") or "1.9.4"
        if self.cfg.public_url:
            ws_url = re.sub(r"^http", "ws", self.cfg.public_url.rstrip("/")) + "/xiaozhi/v1/"
        else:
            scheme = "wss" if request.headers.get("X-Forwarded-Proto", request.scheme) == "https" else "ws"
            ws_url = f"{scheme}://{request.host}/xiaozhi/v1/"
        log.info("OTA de %s (%s) -> %s", clean(request.headers.get("Device-Id", "?")), clean(str(version)), ws_url)
        return web.json_response({
            "server_time": {"timestamp": int(time.time() * 1000),
                            "timezone_offset": time.localtime().tm_gmtoff // 60},
            "firmware": {"version": version, "url": ""},  # mesma versao = sem atualizacao
            "websocket": {"url": ws_url, "token": self.cfg.token, "version": 1},
        })

    async def websocket(self, request: web.Request):
        auth = request.headers.get("Authorization", "")
        expected = f"Bearer {self.cfg.token}" if self.cfg.token else (auth if self.cfg.insecure else "\0")
        if not hmac.compare_digest(auth.encode(), expected.encode()):  # bytes: aceita qualquer cabecalho
            self.warn("token invalido de %s", request.remote)
            raise web.HTTPUnauthorized()
        if not self.device_allowed(request):
            raise web.HTTPForbidden()
        device = clean(request.headers.get("Device-Id", "") or request.remote or "?")
        if old := self.sessions.get(device):  # o aparelho reconectou: so vale a conexao nova
            await old.ws.close()
        if len(self.sessions) >= self.MAX_SESSIONS:
            raise web.HTTPServiceUnavailable()
        ws = web.WebSocketResponse(heartbeat=30, max_msg_size=1 << 20)
        await ws.prepare(request)
        s = Session(ws=ws, device=device, server=self)
        self.sessions[device] = s
        log.info("[%s] conectado", device)
        watchdog = asyncio.create_task(self.close_when_idle(s))
        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    try:
                        data = json.loads(msg.data)
                    except ValueError:
                        continue
                    if not isinstance(data, dict):
                        continue
                    try:
                        await s.on_json(data)
                    except Exception:  # uma mensagem estranha nao derruba a conversa
                        log.exception("[%s] erro tratando %s", device, data.get("type"))
                elif msg.type == WSMsgType.BINARY:
                    s.on_audio(msg.data)
        finally:
            if self.sessions.get(device) is s:
                del self.sessions[device]
            watchdog.cancel()
            await s.cancel()
            log.info("[%s] desconectado", device)
        return ws

    async def close_when_idle(self, s: Session):
        """Sem fala por um tempo: fecha o canal e o aparelho volta a esperar o "Hi, M Five"."""
        while not s.ws.closed:
            await asyncio.sleep(1)
            if not s.busy() and not s.in_speech and time.time() - s.last_activity > self.cfg.idle_close_s:
                log.info("[%s] ocioso, encerrando a conversa", s.device)
                await s.ws.close()

    def _load_models(self, loop):
        try:
            self.voice.load()
            self.asr.load()
        except Exception:
            log.exception("falha ao carregar os modelos")
            os._exit(1)
        loop.call_soon_threadsafe(self.ready.set)

    def run(self):
        app = web.Application()
        ota_path = f"/xiaozhi/ota/{{secret:{re.escape(self.cfg.secret)}}}/" if self.cfg.secret else "/xiaozhi/ota/"
        app.router.add_route("*", ota_path, self.ota)
        app.router.add_get("/xiaozhi/v1/", self.websocket)
        # health check: so responde 200 com os modelos carregados (no deploy, o Render espera por isso)
        app.router.add_get("/", lambda r: web.Response(text="claude-xiaozhi ok\n") if self.ready.is_set()
                           else web.Response(status=503, text="carregando modelos\n"))

        async def on_startup(app):
            loop = asyncio.get_running_loop()
            loop.run_in_executor(None, self._load_models, loop)

        app.on_startup.append(on_startup)
        agent = f"api ({self.agent.model}, esforco {self.cfg.effort})" if self.cfg.agent == "api" else self.cfg.agent
        log.info("agente: %s | pasta: %s | voz: %s/%s | whisper: %s | OTA: %s", agent, self.cfg.workdir,
                 self.cfg.tts, self.cfg.voice, self.cfg.whisper,
                 "protegido por segredo" if self.cfg.secret else "ABERTO em /xiaozhi/ota/")
        # sem log de acesso: a URL do OTA carrega o segredo
        web.run_app(app, host=self.cfg.host, port=self.cfg.port, print=None, access_log=None)


def main():
    env = os.environ.get
    data_default = env("DATA_DIR", str(Path.home() / ".claude-atom"))
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=int(env("PORT", "8003")))
    p.add_argument("--agent", choices=["api", "claude", "opencode"], default=env("AGENT", "api"),
                   help="api: API da Anthropic (ANTHROPIC_API_KEY); claude: Claude Code; opencode: OpenCode")
    p.add_argument("--data-dir", default=data_default, help="modelos, vozes e pasta de trabalho")
    p.add_argument("--workdir", default=env("WORKDIR"), help="pasta onde o agente trabalha (padrao: DATA_DIR/workspace)")
    p.add_argument("--model", default=env("AGENT_MODEL"),
                   help="opus (padrao da api), sonnet, haiku ou um id completo / provider/model no OpenCode")
    p.add_argument("--effort", choices=["low", "medium", "high", "xhigh", "max"], default=env("AGENT_EFFORT", "low"),
                   help="agente api: quanto o Claude pensa antes de responder")
    p.add_argument("--public-url", default=env("PUBLIC_URL", ""),
                   help="endereco publico do servidor (ex.: https://mac.tailnet.ts.net) para o WebSocket do OTA")
    p.add_argument("--tools", default=env("AGENT_TOOLS", Config.tools),
                   help="ferramentas liberadas no Claude Code (o resto e negado; sem Bash/Edit por padrao)")
    p.add_argument("--tts", choices=["piper", "say"], default=env("TTS", "piper"))
    p.add_argument("--voice", default=env("VOICE"), help="voz Piper (pt_BR-faber-medium) ou do macOS (Luciana)")
    p.add_argument("--rate", type=float, default=float(env("VOICE_RATE", "1.0")), help="velocidade da fala")
    p.add_argument("--whisper", default=env("WHISPER_MODEL", "small"), help="tiny, base, small, medium...")
    p.add_argument("--language", default=env("LANGUAGE", "pt"), help="idioma da fala; 'auto' para detectar")
    p.add_argument("--secret", default=env("CLAUDE_ATOM_SECRET", ""), help="segredo do caminho de OTA")
    p.add_argument("--allowed", default=env("ALLOWED_DEVICES", ""), help="MACs autorizados, separados por virgula")
    p.add_argument("--insecure", action="store_true", default=env("INSECURE") == "1",
                   help="aceita rodar sem segredo (OTA aberto em /xiaozhi/ota/): so na rede local")
    p.add_argument("--log-text", action="store_true", default=env("LOG_TEXT") == "1",
                   help="escreve no log o que voce e o Claude disseram (padrao: so o tamanho)")
    p.add_argument("--budget", type=float, default=float(env("BUDGET_USD", "5")),
                   help="agente api: avisa por voz a cada US$ X gastos no periodo (0 desliga)")
    p.add_argument("--budget-period", choices=["day", "month"], default=env("BUDGET_PERIOD", "day"))
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args()
    if not a.secret and not a.insecure:
        p.error("defina CLAUDE_ATOM_SECRET (ou --secret). Para rodar aberto na rede local, use --insecure")

    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, stream=sys.stdout,
                        format="%(asctime)s %(levelname).1s %(message)s", datefmt="%H:%M:%S")
    data_dir = Path(a.data_dir).expanduser()
    workdir = Path(a.workdir).expanduser() if a.workdir else data_dir / "workspace"
    workdir.mkdir(parents=True, exist_ok=True)
    voice = a.voice or ("pt_BR-faber-medium" if a.tts == "piper" else "Luciana")
    cfg = Config(port=a.port, agent=a.agent, data_dir=data_dir, workdir=workdir, model=a.model, effort=a.effort,
                 public_url=a.public_url, tools=a.tools,
                 tts=a.tts, voice=voice, rate=a.rate, whisper=a.whisper,
                 language=None if a.language == "auto" else a.language, secret=a.secret,
                 insecure=a.insecure, log_text=a.log_text, budget_usd=a.budget, budget_period=a.budget_period,
                 allowed=frozenset(m.strip().lower() for m in a.allowed.split(",") if m.strip()))
    Server(cfg).run()


if __name__ == "__main__":
    main()

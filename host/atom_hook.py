#!/usr/bin/env python3
"""Hook do Claude Code que espelha a sessao no AtomS3 (via host/bridge.py).

Configure o mesmo comando para todos os eventos; ele le hook_event_name do
stdin. Nunca atrapalha o Claude Code: se a ponte ou o device nao estiverem
disponiveis, sai com 0 sem imprimir nada.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~/.claude-atom")
SOCK_PATH = os.path.join(HOME, "bridge.sock")
PERM_TTL = int(os.environ.get("CLAUDE_ATOM_PERM_TIMEOUT", "50"))


def short(text, limit=200):
    text = " ".join(str(text).split())
    return text if len(text) <= limit else text[: limit - 3] + "..."


def describe_tool(tool, ti):
    """Resumo legivel do que a ferramenta vai fazer, para caber em 128x128."""
    ti = ti or {}
    if tool == "Bash":
        return ti.get("command", "")
    if tool in ("Read", "Write", "Edit", "MultiEdit", "NotebookEdit"):
        path = ti.get("file_path") or ti.get("notebook_path") or ""
        return os.path.basename(path) or path
    if tool in ("Grep", "Glob"):
        return ti.get("pattern", "")
    if tool == "WebFetch":
        return ti.get("url", "")
    if tool == "WebSearch":
        return ti.get("query", "")
    if tool in ("Agent", "Task"):
        return ti.get("description", "")
    for key in ("command", "file_path", "path", "url", "query", "description", "prompt"):
        if key in ti:
            return str(ti[key])
    return json.dumps(ti, ensure_ascii=False)


def tool_title(tool):
    if tool.startswith("mcp__"):
        return tool.split("__")[-1]
    return tool


def ensure_bridge():
    """Sobe a ponte em segundo plano se ela ainda nao estiver rodando."""
    if os.path.exists(SOCK_PATH):
        return
    uv = shutil.which("uv") or "/opt/homebrew/bin/uv"
    os.makedirs(HOME, exist_ok=True)
    with open(os.path.join(HOME, "bridge.log"), "a") as logf:
        subprocess.Popen(
            [uv, "run", "--quiet", "--script", os.path.join(HERE, "bridge.py")],
            stdin=subprocess.DEVNULL,
            stdout=logf,
            stderr=logf,
            start_new_session=True,
        )
    for _ in range(40):  # ate ~4s na primeira vez (uv resolve o pyserial)
        if os.path.exists(SOCK_PATH):
            return
        time.sleep(0.1)


def talk(msg, wait_reply=False, timeout=1.5):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(SOCK_PATH)
    except OSError:
        # socket velho de uma ponte que morreu: apaga e tenta subir de novo
        try:
            os.unlink(SOCK_PATH)
        except OSError:
            pass
        ensure_bridge()
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(timeout)
            s.connect(SOCK_PATH)
        except OSError:
            return None
    with s:
        s.sendall((json.dumps(msg) + "\n").encode())
        if not wait_reply:
            return None
        try:
            return json.loads(s.makefile("rb").readline() or b"null")
        except (OSError, ValueError):
            return None


def state(s, project, title="", m=""):
    talk({"t": "state", "s": s, "p": project, "title": title, "m": short(m)})


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return
    event = data.get("hook_event_name", "")
    project = os.path.basename(data.get("cwd") or os.getcwd())
    tool = data.get("tool_name", "")

    ensure_bridge()

    if event == "SessionStart":
        state("idle", project, "Claude Code", "Sessao iniciada")
    elif event == "UserPromptSubmit":
        state("thinking", project, "Pensando", data.get("prompt", ""))
    elif event == "PreToolUse":
        state("tool", project, tool_title(tool), describe_tool(tool, data.get("tool_input")))
    elif event in ("PostToolUse", "PostToolUseFailure"):
        state("thinking", project, "Pensando", f"{tool_title(tool)} ok")
    elif event == "Notification":
        state("waiting", project, data.get("title") or "Sua vez", data.get("message", ""))
    elif event == "Stop":
        state("done", project, "Pronto", data.get("last_assistant_message", ""))
    elif event == "SessionEnd":
        state("idle", project, "Claude Code", "Sessao encerrada")
    elif event == "PermissionRequest":
        reply = talk(
            {
                "t": "perm",
                "tool": tool_title(tool),
                "m": short(describe_tool(tool, data.get("tool_input")), 240),
                "p": project,
                "ttl": PERM_TTL,
            },
            wait_reply=True,
            timeout=PERM_TTL + 5,
        )
        decision = (reply or {}).get("d")
        if decision in ("allow", "deny"):
            out = {"behavior": decision}
            if decision == "deny":
                out["message"] = "Negado pelo usuario no AtomS3."
            print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": out}}))
        # ask / timeout / offline: sem saida, o Claude Code pergunta no terminal


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass  # o painel e um acessorio; nunca quebrar a sessao

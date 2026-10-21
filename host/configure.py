#!/usr/bin/env python3
"""Configura o Claude Atom pela USB (Wi-Fi, chaves de API, modelo, voz).

  python3 host/configure.py          pergunta tudo (senhas/chaves sem eco)
  python3 host/configure.py --info   mostra o estado do aparelho

Chaves podem vir de ANTHROPIC_API_KEY e GOOGLE_API_KEY no ambiente.
Campos deixados em branco nao sao alterados.
"""

import getpass
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from atom_hook import ensure_bridge, talk  # noqa: E402

WIFI_STATES = ["conectando", "online", "offline", "portal de configuracao"]


def info():
    reply = talk({"t": "info"}, wait_reply=True, timeout=8)
    if not reply or "error" in (reply or {}):
        sys.exit(f"Sem resposta do aparelho: {(reply or {}).get('error', 'ponte indisponivel')}")
    return reply


def show(i):
    state = WIFI_STATES[i["wifi"]] if 0 <= i.get("wifi", -1) < len(WIFI_STATES) else i.get("wifi")
    print(f"Firmware : {i.get('fw')}")
    print(f"Wi-Fi    : {state} {i.get('ssid') or ''} {i.get('ip') or ''}".rstrip())
    if i.get("ap"):
        print(f"Portal   : {i['ap']}")
    print(f"Modelo   : {i.get('model')}")
    print(f"Chaves   : Anthropic {'ok' if i.get('anthropic') else 'faltando'}, "
          f"Google {'ok' if i.get('google') else 'faltando'}")
    print(f"Memoria  : {i.get('heap')} livres (minimo {i.get('heap_min')})")


def main():
    ensure_bridge()
    if "--info" in sys.argv:
        show(info())
        return

    show(info())
    print("\nDeixe em branco para manter o valor atual.\n")
    cfg = {"t": "config"}
    ssid = input("Rede Wi-Fi: ").strip()
    if ssid:
        cfg["ssid"] = ssid
        cfg["pass"] = getpass.getpass("Senha do Wi-Fi: ")
    for field, env, label in (("anthropic", "ANTHROPIC_API_KEY", "Chave Anthropic"),
                              ("google", "GOOGLE_API_KEY", "Chave Google Cloud")):
        value = os.environ.get(env) or getpass.getpass(f"{label} (sk-.../AIza...): ").strip()
        if value:
            cfg[field] = value
    model = input("Modelo [claude-opus-5-5 | claude-sonnet-5-5 | claude-haiku-5-5]: ").strip()
    if model:
        cfg["model"] = model
    voice = input("Voz do Google (ex.: pt-BR-Neural2-B; vazio = padrao): ").strip()
    if voice:
        cfg["voice"] = voice

    if len(cfg) == 1:
        print("Nada para mudar.")
        return
    reply = talk(cfg, wait_reply=True, timeout=8)
    if reply and reply.get("t") == "config_ok":
        print("\nSalvo. O aparelho esta reiniciando e vai conectar ao Wi-Fi.")
    else:
        sys.exit(f"Falhou: {reply}")


if __name__ == "__main__":
    main()

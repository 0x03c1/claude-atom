# Claude Atom

M5Stack AtomS3 + Atomic Echo Base conversando por voz com o **Claude**, pela API da Anthropic.

O aparelho roda o firmware **xiaozhi 1.9.4** (palavra de ativação "Hi, M Five", áudio Opus, tela) apontando
para um servidor no próprio Mac em vez da nuvem da Tenclass (api.tenclass.net):

```
AtomS3 ── "Hi, M Five" / botão ── Opus 16 kHz ──► Tailscale Funnel (HTTPS público) ──► Mac: server/claude_xiaozhi.py
                                                     ├─ VAD (webrtcvad) + Whisper local (faster-whisper)
                                                     ├─ API da Anthropic em streaming (Claude Opus 5.5 + busca na web)
                                                     └─ voz Piper pt-BR ─► Opus 24 kHz ─► alto-falante
```

A API da Anthropic só troca texto, por isso transcrição e voz rodam no Mac. Nada de VPS: o Funnel do
Tailscale dá ao Mac um endereço HTTPS fixo (`https://<mac>.<tailnet>.ts.net`), e o aparelho funciona em
qualquer Wi-Fi enquanto o Mac estiver ligado e acordado.

## Instalar no Mac

1. Guarde a chave da API no Keychain (ela não fica em arquivo nenhum):
   ```sh
   security add-generic-password -U -s claude-atom -a anthropic -w
   ```
2. Deixe o servidor sempre ligado e publicado pelo Funnel:
   ```sh
   server/install-mac.sh            # LaunchAgent + Funnel; na 1ª vez o Tailscale pode pedir para liberar o Funnel
   tail -f ~/.claude-atom/server.log
   server/install-mac.sh --remove   # desfaz tudo
   ```
3. Compile e grave o firmware com a URL do Funnel (ver **Firmware**).

O acesso é protegido pelo segredo da URL de OTA, pelo token do WebSocket e pela lista de MACs, todos em
`~/.claude-atom/cloud.json` (fora do repositório).

## Usar

Diga **"Hi, M Five"** ou clique no botão e fale. A conversa continua sozinha; depois de 30 s de silêncio o
aparelho volta a esperar a palavra de ativação. Clique durante a fala para interromper. Diga "nova conversa"
para zerar o contexto (ele também zera após 15 min sem uso).

Opções (em `server/run-mac.sh ...` ou direto no `claude_xiaozhi.py`):

- `--model opus|sonnet|haiku` (padrão `opus` = `claude-opus-5-5`); `--effort low|medium|high` (padrão `low`, mais rápido)
- `--agent claude`: usa o Claude Code logado (`claude -p`), sem chave de API e com leitura de arquivos;
  `--agent opencode` para o OpenCode
- `--voice`, `--rate`, `--whisper small|base|medium`, `--tts say`

## Firmware

`xiaozhi-claude/` é o xiaozhi v1.9.4 com a interface do Claude (`main/boards/atoms3-echo-base/claude_display.*`),
strings pt-BR, só a palavra "Hi, M Five" e o ponto de acesso "Claude-XXXX". Base: tag `v1.9.4` de
[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (commit `3ced770`); as mudanças estão em
`main/boards/atoms3-echo-base/`, `main/boards/common/wifi_board.cc`, `main/idf_component.yml` e
`main/assets/locales/pt-PT/language.json`. A URL de OTA vem de `public_url`
e `secret` em `~/.claude-atom/cloud.json`:

```sh
xiaozhi-claude/build-claude.sh
cd xiaozhi-claude && source env.sh && idf.py -p /dev/cu.usbmodem12301 flash   # preserva o Wi-Fi salvo
```

Backups da flash em `backup/` (contêm a senha do Wi-Fi; fora do git):

```sh
# xiaozhi original com OTA para http://192.168.0.3:8003/xiaozhi/ota/ (servidor sem --secret, só na rede local)
uvx --from esptool esptool --port /dev/cu.usbmodem12301 write-flash 0 backup/xiaozhi-claude-8MB.bin
# voltar ao original (nuvem Tenclass)
uvx --from esptool esptool --port /dev/cu.usbmodem12301 write-flash 0 backup/atoms3-original-8MB.bin
```

## Outras tentativas neste repositório

- `firmware/` + `host/`: firmware Arduino próprio (painel do Claude Code via USB e assistente com APIs na nuvem).
  Os hooks do painel estão desativados em `.claude/settings.painel-usb.json.disabled`.
- `Dockerfile` + `render.yaml`: o mesmo servidor em container, se um dia precisar rodar fora do Mac (ex.: Render).

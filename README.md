# Claude Atom

**Jarvis**: um M5Stack **AtomS3 + Atomic Echo Base** conversando por voz com o **Claude**, pela API da Anthropic.

O aparelho roda o firmware [xiaozhi](https://github.com/78/xiaozhi-esp32) (palavra de ativação "Jarvis",
áudio Opus, tela) com uma interface do Claude e aponta para um servidor próprio em vez da nuvem do xiaozhi:

```
AtomS3 ── "Jarvis" / botão ────── Opus 16 kHz ──► server/claude_xiaozhi.py (Render, ou o seu computador)
                                                     ├─ VAD (webrtcvad) + Whisper (faster-whisper, na CPU)
                                                     ├─ API da Anthropic em streaming (Claude + busca na web
                                                     │  + ferramentas do aparelho: volume, tela, estado)
                                                     └─ voz Piper pt-BR ─► Opus 24 kHz ─► alto-falante
```

A API da Anthropic só troca texto, por isso a transcrição e a voz rodam no servidor.

## Usar

Diga **"Oi, Jarvis"** (o aparelho reconhece a palavra "Jarvis", em inglês) ou clique no botão e fale. A conversa continua sozinha; depois de 30 s de silêncio o
aparelho volta a esperar a palavra de ativação. **Clique durante a fala para interromper.** Diga "nova conversa"
para zerar o contexto (ele também zera após 15 min sem uso). Dá para pedir "abaixa o volume" ou "deixa a tela
mais clara".

## Servidor no Render

1. Faça um fork deste repositório e, no [Render](https://dashboard.render.com), crie um **Blueprint** apontando
   para ele (ele lê o `render.yaml`) ou um **Web Service** com runtime Docker e o `Dockerfile` da raiz.
2. Use uma instância de **2 GB** (`1c-2g`): o Whisper `small` ocupa ~420 MB parado e mais ao transcrever; com
   512 MB o serviço cai por falta de memória. O plano grátis dorme após 15 min e o aparelho dá erro ao ligar.
3. Variáveis de ambiente:

   | Variável | Valor |
   |---|---|
   | `ANTHROPIC_API_KEY` | a sua chave da API |
   | `CLAUDE_ATOM_SECRET` | um segredo seu, por exemplo `python3 -c "import secrets;print(secrets.token_hex(16))"` |
   | `ALLOWED_DEVICES` | o MAC do aparelho (aparece no log serial: `wifi:mode : sta (xx:xx:...)`) |
   | `AGENT_MODEL` | `opus` (padrão), `sonnet` ou `haiku` (mais rápido e barato) |
   | `AGENT_EFFORT` | `low` (padrão), `medium` ou `high` |
   | `WHISPER_MODEL` | `small` (padrão, mais preciso) ou `base` (~3x mais rápido) |
   | `ASSISTANT_NAME` | nome do assistente (padrão `Jarvis`; a palavra de ativação é fixa no firmware) |
   | `USER_TITLE` | como ele chama você (padrão `senhor`; vazio para nenhum) |
   | `LOG_TEXT` | `1` para escrever no log o que foi dito (padrão: só o tamanho) |
   | `BUDGET_USD` | aviso por voz a cada US$ X de gasto estimado no período (padrão `5`; `0` desliga) |
   | `BUDGET_PERIOD` | `day` (padrão, zera à meia-noite) ou `month` |

Cada push na `main` que mexa no servidor gera um deploy novo sem derrubar o que está no ar.

O aviso de gasto é uma estimativa feita a partir do uso de tokens de cada resposta e fica na memória (um deploy
zera a conta). Para um teto de verdade, defina o limite de gasto no Console da Anthropic.

## Firmware

`xiaozhi-claude/` é o xiaozhi **v1.9.4** ([78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), commit
`3ced770`, licença MIT) com a interface do Claude (`main/boards/atoms3-echo-base/claude_display.*`), strings pt-BR,
só a palavra "Jarvis" (`wn9_jarvis_tts` do ESP-SR) e o ponto de acesso "Jarvis-XXXX". As mudanças estão em `main/boards/atoms3-echo-base/`,
`main/boards/common/wifi_board.cc`, `main/idf_component.yml` e `main/assets/locales/pt-PT/language.json`.

A URL do servidor fica gravada no firmware. Crie `~/.claude-atom/cloud.json` (fora do repositório) e compile:

```sh
cat > ~/.claude-atom/cloud.json <<'EOF'
{"public_url": "https://SEU-SERVICO.onrender.com", "secret": "O MESMO CLAUDE_ATOM_SECRET", "device": "xx:xx:xx:xx:xx:xx"}
EOF
xiaozhi-claude/build-claude.sh                      # precisa do ESP-IDF 5.x (ajuste xiaozhi-claude/env.sh)
cd xiaozhi-claude && source env.sh && idf.py -p /dev/cu.usbmodem* flash   # preserva o Wi-Fi salvo
```

Na primeira vez o aparelho abre o ponto de acesso "Jarvis-XXXX" para você escolher o Wi-Fi.

## Segurança

- O servidor só atende quem conhece o segredo: a URL de OTA (`/xiaozhi/ota/<segredo>/`) responde 404 para o resto
  e entrega o token do WebSocket, que é derivado do segredo. Sem segredo o servidor nem sobe (a não ser com
  `--insecure`, só para a rede local).
- `ALLOWED_DEVICES` é uma camada extra; o MAC não é secreto. Quem tiver o segredo pode usar o seu crédito da API,
  então trate-o como senha. O segredo fica no firmware gravado, no `~/.claude-atom/cloud.json` e no Render.
- O Claude só alcança uma lista fechada de ferramentas do aparelho (estado, volume, tela); as demais, como gravar
  firmware, ficam de fora.
- O histórico da conversa fica só na memória do servidor. Os logs não guardam o texto, a menos que `LOG_TEXT=1`.

## Rodar no seu computador

```sh
uv run --script server/claude_xiaozhi.py --secret "$CLAUDE_ATOM_SECRET" --allowed xx:xx:xx:xx:xx:xx
```

Opções: `--model`, `--effort`, `--whisper`, `--voice`, `--rate`, `--tts say` (voz do macOS),
`--agent claude` (Claude Code logado, sem chave de API) ou `--agent opencode`. No Mac, `server/install-mac.sh`
deixa o servidor sempre ligado e o publica pelo Tailscale Funnel (`server/run-mac.sh` lê a chave do Keychain).

## Outras tentativas neste repositório

- `firmware/` + `host/`: um firmware Arduino anterior (painel do Claude Code via USB). **Descontinuado**: a interface
  web dele na rede local não tem autenticação; não use.

## Contribuir

Issues e pull requests são bem-vindos. Ideias em aberto: interromper o Jarvis falando "Jarvis" durante a
resposta, suporte ao AtomS3R (com PSRAM: redução de ruído e cancelamento de eco) e transcrição mais rápida.

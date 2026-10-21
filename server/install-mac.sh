#!/usr/bin/env bash
# Deixa o servidor sempre ligado no Mac e acessivel de fora de casa:
#   1. LaunchAgent: sobe no login e reinicia se cair (log em ~/.claude-atom/server.log)
#   2. Tailscale Funnel: https://<mac>.<tailnet>.ts.net -> localhost:8003 (HTTPS publico, sem VPS)
# Desfazer: server/install-mac.sh --remove
set -euo pipefail
cd "$(dirname "$0")"

LABEL=com.claude-atom.server
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
CFG="$HOME/.claude-atom/cloud.json"
PORT=8003

if [ "${1:-}" = "--remove" ]; then
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  rm -f "$PLIST"
  tailscale funnel --https=443 off 2>/dev/null || true
  echo "Servidor e Funnel desligados."
  exit 0
fi

# endereco publico do Mac no Tailscale, guardado para o servidor e para o firmware
HOST=$(tailscale status --json | python3 -c "import json,sys;print(json.load(sys.stdin)['Self']['DNSName'].rstrip('.'))")
python3 - "$CFG" "https://$HOST" <<'PY'
import json, sys
path, url = sys.argv[1], sys.argv[2]
cfg = json.load(open(path))
cfg["public_url"] = url
json.dump(cfg, open(path, "w"), indent=2)
PY

mkdir -p "$(dirname "$PLIST")"
cat > "$PLIST" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$PWD/run-mac.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>StandardOutPath</key><string>$HOME/.claude-atom/server.log</string>
  <key>StandardErrorPath</key><string>$HOME/.claude-atom/server.log</string>
</dict>
</plist>
XML
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"

# na primeira vez o Tailscale pode pedir para liberar o Funnel no painel (abre um link)
tailscale funnel --bg --https=443 "http://127.0.0.1:$PORT"
echo
echo "Servidor: https://$HOST/  (log: tail -f ~/.claude-atom/server.log)"

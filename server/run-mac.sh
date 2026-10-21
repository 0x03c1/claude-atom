#!/usr/bin/env bash
# Sobe o servidor no Mac com a configuracao de ~/.claude-atom/cloud.json e a chave do Keychain.
# Usado pelo LaunchAgent (install-mac.sh), mas tambem roda na mao: server/run-mac.sh [opcoes extras]
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

CFG="$HOME/.claude-atom/cloud.json"
get() { python3 -c "import json,sys;print(json.load(open('$CFG')).get(sys.argv[1],''))" "$1"; }

if [ -z "${ANTHROPIC_API_KEY:-}" ]; then
  ANTHROPIC_API_KEY=$(security find-generic-password -s claude-atom -a anthropic -w 2>/dev/null || true)
  export ANTHROPIC_API_KEY
fi
if [ -z "$ANTHROPIC_API_KEY" ]; then
  echo "Falta a chave da API. Guarde no Keychain com:"
  echo "  security add-generic-password -U -s claude-atom -a anthropic -w"
fi

exec uv run --script claude_xiaozhi.py \
  --secret "$(get secret)" --allowed "$(get device)" --public-url "$(get public_url)" "$@"

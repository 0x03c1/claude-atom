#!/usr/bin/env bash
# Compila o xiaozhi para AtomS3 + Atomic Echo Base com a interface do Jarvis (palavra "Jarvis") e o servidor.
# A URL de OTA vem de ~/.claude-atom/cloud.json (fora do repositorio; public_url e gravado por
# server/install-mac.sh):  {"public_url": "https://<mac>.<tailnet>.ts.net", "secret": "<hex>", "device": "<mac>"}
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh

CFG="$HOME/.claude-atom/cloud.json"
OTA_URL=$(python3 -c "import json;c=json.load(open('$CFG'));print(f\"{c['public_url']}/xiaozhi/ota/{c['secret']}/\")")

if [ ! -f sdkconfig ]; then
  idf.py set-target esp32s3
  cat >> sdkconfig <<CONF
# --- Claude Atom ---
CONFIG_BOARD_TYPE_ATOMS3_ECHO_BASE=y
CONFIG_SPIRAM=n
CONFIG_USE_AFE=n
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v1/8m.csv"
CONFIG_LANGUAGE_PT_PT=y
CONFIG_SR_WN_WN9_JARVIS_TTS=y
# CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS is not set
CONF
fi
# palavra de ativacao: so "Jarvis" (reaplicada sempre, para sdkconfig ja existente)
sed -i '' -E '/^(# )?CONFIG_SR_WN_WN9_(HIMFIVE|JARVIS_TTS)[ =]/d' sdkconfig
printf '%s\n' '# CONFIG_SR_WN_WN9_HIMFIVE is not set' 'CONFIG_SR_WN_WN9_JARVIS_TTS=y' >> sdkconfig
# a URL e sempre reaplicada (o segredo pode mudar)
sed -i '' '/^CONFIG_OTA_URL=/d' sdkconfig
echo "CONFIG_OTA_URL=\"$OTA_URL\"" >> sdkconfig
sed -i '' 's/^CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y/# CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS is not set/' sdkconfig

idf.py build
echo "Gravar: source env.sh && idf.py -p /dev/cu.usbmodem* flash"

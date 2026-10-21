#!/usr/bin/env bash
# Compila o xiaozhi para AtomS3 + Atomic Echo Base com a interface do Jarvis e o servidor.
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
# sem palavra de ativacao: no ESP32-S3 sem PSRAM (AtomS3) o detector do ESP-SR nao roda
# (o AFE exige PSRAM e o WakeNet sem AFE trava ao carregar o modelo); conversa pelo botao
sed -i '' -E '/^(# )?CONFIG_USE_ESP_WAKE_WORD[ =]/d' sdkconfig
# a URL e sempre reaplicada (o segredo pode mudar)
sed -i '' '/^CONFIG_OTA_URL=/d' sdkconfig
echo "CONFIG_OTA_URL=\"$OTA_URL\"" >> sdkconfig
sed -i '' 's/^CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y/# CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS is not set/' sdkconfig

idf.py build
echo "Gravar: source env.sh && idf.py -p /dev/cu.usbmodem* flash"

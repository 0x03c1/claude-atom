# Ambiente do ESP-IDF 5.5 sem o export.sh (removemos o toolchain RISC-V, inutil aqui).
#   source env.sh && idf.py build
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_TOOLS_PATH="$HOME/.espressif"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env/idf5.5_py3.13_env"
export ESP_ROM_ELF_DIR="$(ls -d "$IDF_TOOLS_PATH"/tools/esp-rom-elfs/*/ 2>/dev/null | head -1)"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$(ls -d "$IDF_TOOLS_PATH"/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1):$(ls -d "$IDF_TOOLS_PATH"/tools/esp32ulp-elf/*/esp32ulp-elf/bin | head -1):$IDF_PATH/tools:$PATH"

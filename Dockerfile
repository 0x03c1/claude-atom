# Servidor claude-xiaozhi em container (Render; ver render.yaml). Contexto: raiz do repositorio.
# Precisa de ANTHROPIC_API_KEY, CLAUDE_ATOM_SECRET e ALLOWED_DEVICES no ambiente.
FROM python:3.12-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libopus0 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY server/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY server/claude_xiaozhi.py .

# Whisper e a voz Piper vao dentro da imagem: o servidor sobe em segundos e nao precisa de disco
ARG WHISPER_MODEL=small
ENV DATA_DIR=/app/data \
    HF_HOME=/app/hf \
    WHISPER_MODEL=$WHISPER_MODEL \
    PYTHONUNBUFFERED=1
RUN python -c "from pathlib import Path; import claude_xiaozhi as c; \
c.Voice(c.Config(data_dir=Path('/app/data'))).load(); c.Transcriber('$WHISPER_MODEL', 'pt').load()"
# o modelo ja esta na imagem: nao consultar o Hugging Face ao subir
ENV HF_HUB_OFFLINE=1

EXPOSE 8003
CMD ["python", "claude_xiaozhi.py", "--agent", "api", "--tts", "piper"]

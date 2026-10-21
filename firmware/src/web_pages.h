#pragma once
// Paginas servidas pelo proprio AtomS3.

static const char CHAT_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Claude Atom</title>
<style>
:root{--bg:#faf9f5;--fg:#141413;--dim:#6b6a65;--card:#fff;--line:#e3e1d8;--acc:#d97757}
@media (prefers-color-scheme:dark){:root{--bg:#1f1e1d;--fg:#f0eee6;--dim:#a3a197;--card:#2a2927;--line:#3a3936}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,sans-serif}
main{max-width:640px;margin:0 auto;padding:16px;display:flex;flex-direction:column;min-height:100vh}
header{display:flex;align-items:center;gap:8px;margin-bottom:12px}
header b{font-size:18px}header span{color:var(--dim);font-size:13px;margin-left:auto}
#log{flex:1;display:flex;flex-direction:column;gap:10px;padding-bottom:12px}
.m{padding:10px 12px;border-radius:12px;max-width:85%;white-space:pre-wrap;word-wrap:break-word}
.u{align-self:flex-end;background:var(--acc);color:#fff}.a{align-self:flex-start;background:var(--card);border:1px solid var(--line)}
.e{align-self:center;color:#c0392b;font-size:14px}
form{display:flex;gap:8px;position:sticky;bottom:0;background:var(--bg);padding:8px 0}
input{flex:1;padding:12px;border-radius:10px;border:1px solid var(--line);background:var(--card);color:var(--fg);font-size:16px}
button{padding:0 16px;border-radius:10px;border:0;background:var(--acc);color:#fff;font-size:16px}
button.s{background:transparent;color:var(--dim);border:1px solid var(--line)}
a{color:var(--dim);font-size:13px}
</style></head><body><main>
<header><b>Claude Atom</b><span id="st">...</span></header>
<div id="log"></div>
<form id="f"><input id="q" placeholder="Pergunte algo (ou segure o botao do Atom)" autocomplete="off">
<button>Enviar</button><button type="button" class="s" id="r" title="Nova conversa">&#8634;</button></form>
<a href="/config">Configuracoes</a>
</main><script>
const log=document.getElementById('log'),st=document.getElementById('st');
const names={idle:'pronto',listening:'ouvindo...',transcribing:'entendendo...',thinking:'pensando...',speaking:'falando...',reply:'pronto',error:'erro'};
let seq=-1,last={};
function bubble(c,t){const d=document.createElement('div');d.className='m '+c;d.textContent=t;log.appendChild(d);return d}
async function poll(){
 try{const s=await (await fetch('/api/state')).json();st.textContent=names[s.phase]||s.phase;
  if(s.seq!==seq){seq=s.seq;if(s.q){bubble('u',s.q);last.a=bubble('a','')}}
  if(last.a&&s.a!==last.a.textContent){last.a.textContent=s.a||'...';scrollTo(0,document.body.scrollHeight)}
  if(s.phase==='error'&&s.err&&last.err!==s.err+s.since){last.err=s.err+s.since;bubble('e',s.err)}
 }catch(e){st.textContent='sem conexao'}
 setTimeout(poll,400)}
document.getElementById('f').onsubmit=async e=>{e.preventDefault();const q=document.getElementById('q');if(!q.value.trim())return;
 const r=await fetch('/api/ask',{method:'POST',body:q.value});if(!r.ok)bubble('e',await r.text());else q.value=''};
document.getElementById('r').onclick=async()=>{await fetch('/api/reset',{method:'POST'});log.innerHTML='';last={}};
poll();
</script></body></html>)HTML";

// %SSID% etc. sao substituidos no envio.
static const char CONFIG_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Claude Atom</title>
<style>
:root{--bg:#faf9f5;--fg:#141413;--dim:#6b6a65;--card:#fff;--line:#e3e1d8;--acc:#d97757}
@media (prefers-color-scheme:dark){:root{--bg:#1f1e1d;--fg:#f0eee6;--dim:#a3a197;--card:#2a2927;--line:#3a3936}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,sans-serif}
main{max-width:520px;margin:0 auto;padding:16px}h1{font-size:20px}
label{display:block;margin:14px 0 4px;font-weight:600}small{color:var(--dim);display:block;font-weight:400}
input,select{width:100%;padding:11px;border-radius:10px;border:1px solid var(--line);background:var(--card);color:var(--fg);font-size:16px}
button{margin-top:20px;width:100%;padding:13px;border-radius:10px;border:0;background:var(--acc);color:#fff;font-size:16px}
a{color:var(--dim)}
</style></head><body><main>
<h1>Configurar Claude Atom</h1>
<form method="POST" action="/config">
<label>Rede Wi-Fi</label><input name="ssid" value="%SSID%" list="nets" required><datalist id="nets">%NETS%</datalist>
<label>Senha do Wi-Fi <small>deixe vazio para manter a atual</small></label><input name="pass" type="password">
<label>Chave da API Anthropic <small>%AKEY%</small></label><input name="anthropic" type="password" placeholder="sk-ant-...">
<label>Chave da API Google Cloud <small>%GKEY% &middot; com Speech-to-Text e Text-to-Speech ativadas</small></label><input name="google" type="password">
<label>Modelo</label><select name="model">%MODELS%</select>
<label>Voz do Google <small>vazio = padrao do idioma (ex.: pt-BR-Neural2-B)</small></label><input name="voice" value="%VOICE%">
<label>Idioma</label><input name="lang" value="%LANG%">
<label>Volume (0-255)</label><input name="volume" type="number" min="0" max="255" value="%VOLUME%">
<label><input name="speak" type="checkbox" value="1" style="width:auto" %SPEAK%> Falar as respostas</label>
<button>Salvar e reiniciar</button>
</form>
<p><a href="/">&larr; voltar</a></p>
</main></body></html>)HTML";

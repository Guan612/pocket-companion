// 网页设置页（浏览器打开 http://设备IP/ 看到的那一页）。
//
// 结构照 Cherry Studio：先管供应商（地址 + Key + 它的模型），再给对话 / 带图提问 / 语音识别 / 语音合成
// 各选一个"供应商 / 模型"。页面是静态的，打开后从 /config.json 读设置，保存时整份 JSON 提交到 /save，
// 模型列表经 /models 由设备代拉（浏览器直连供应商会被跨域拦）。接口见 direct_api_config_server.cc。
//
// 设备和浏览器模拟器共用这一份：tools/device-sim/serve.py 按 kDirectApiConfigPage 截出来原样返回，
// 所以改页面只改这里，模拟器里看到的就是设备上的。
#pragma once

inline constexpr char kDirectApiConfigPage[] = R"HTML(<!doctype html>
<html lang="zh">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pocket Companion 设置</title>
<style>
:root{--bg:#111318;--card:#1b1e26;--line:#2c3040;--fg:#e8eaf0;--muted:#8b90a0;--accent:#6aa6ff;--ok:#5ad19a;--err:#ff7a7a}
*{box-sizing:border-box}
body{margin:0;padding:16px 16px 88px;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}
main{max-width:560px;margin:auto}
h1{font-size:20px;margin:0 0 4px}
h2{font-size:15px;color:var(--accent);margin:22px 0 8px;font-weight:600}
p,.hint{color:var(--muted);margin:4px 0;overflow-wrap:anywhere}
.hint{font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;margin:0 0 10px}
.prov-head{display:flex;justify-content:space-between;gap:8px;align-items:center;cursor:pointer}
.prov-head b{font-weight:600}
.tag{font-size:12px;padding:1px 8px;border-radius:999px;background:#2b3142;color:var(--muted);white-space:nowrap}
.tag.ok{color:var(--ok)}.tag.err{color:var(--err)}
label{display:block;margin:10px 0 0;color:var(--muted);font-size:14px}
input,select,textarea{display:block;width:100%;margin-top:4px;padding:8px;border-radius:8px;border:1px solid var(--line);background:#14161c;color:var(--fg);font:inherit}
label.speak{display:flex;align-items:center;gap:8px;cursor:pointer}
label.speak input{width:auto;margin:0;accent-color:var(--accent)}
button{font:inherit;color:var(--fg);background:#2b3142;border:1px solid var(--line);border-radius:8px;padding:7px 12px;cursor:pointer}
button.primary{background:var(--accent);color:#0b1220;border:0;font-weight:600}
button.danger{color:var(--err)}
button:disabled{opacity:.5}
.row{display:flex;gap:8px;align-items:center;margin-top:8px;flex-wrap:wrap}
.row input{flex:1;min-width:0;margin:0}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:6px}
.chip{display:inline-flex;gap:6px;align-items:center;padding:3px 4px 3px 10px;border-radius:999px;background:#232838;font-size:13px;max-width:100%}
.chip span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.chip button{padding:0 6px;border:0;background:none;color:var(--muted)}
.picker{max-height:260px;overflow:auto;margin-top:8px;border:1px solid var(--line);border-radius:8px}
.picker div{display:flex;justify-content:space-between;gap:8px;padding:7px 10px;border-bottom:1px solid var(--line);cursor:pointer;font-size:13px}
.picker div.on{background:#20304d}
.presets{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.presets button{text-align:left;padding:10px}
.presets small{display:block;color:var(--muted);font-size:12px}
.save{position:fixed;left:0;right:0;bottom:0;padding:12px 16px;background:rgba(17,19,24,.95);border-top:1px solid var(--line)}
.save div{max-width:560px;margin:auto;display:flex;gap:10px;align-items:center}
.save button{flex:none;padding:10px 22px}
#msg{flex:1;font-size:14px}
.ok{color:var(--ok)}.err{color:var(--err)}
[hidden]{display:none!important}
</style>
<main>
<h1>Pocket Companion 设置</h1>
<p>先添加供应商（填地址和 Key），再给下面四个用途各选一个模型。接口都是 OpenAI 兼容格式；语音合成要能返回 opus。</p>

<h2>供应商</h2>
<div id="providers"></div>
<button id="add">+ 添加供应商</button>
<div class="card" id="presetBox" hidden>
  <p>选一家，地址和常用模型会自动填好，你只需要填 Key：</p>
  <div class="presets" id="presets"></div>
</div>

<h2>用途</h2>
<div class="card">
  <label>对话模型<select id="r_chat"></select></label>
  <label>带图提问（视觉模式）<select id="r_vision"></select></label>
  <p class="hint">对话模型本身能看图就选"和对话模型相同"。</p>
  <label>语音识别<select id="r_asr"></select></label>
  <label>识别语言<input id="asr_lang" placeholder="zh"></label>
  <label>语音合成<select id="r_tts"></select></label>
  <p class="hint">合成要能返回 opus。按官方文档，目前支持的只有硅基流动和 OpenAI。</p>
  <label>音色<input id="tts_voice" list="voices" placeholder="alloy"></label>
  <datalist id="voices"></datalist>
  <label class="speak"><input type="checkbox" id="tts_speak" checked>语音播报（不勾就只显示文字、不调用合成）</label>
  <p class="err" id="missing" hidden></p>
</div>

<h2>人设提示词</h2>
<textarea id="prompt" rows="4" placeholder="留空用默认提示词"></textarea>

<h2>长期记忆</h2>
<p class="hint">对话里它会自己往里记（一行一条）；这里能看到全部并手改，删掉就不记得了。<span id="memSize"></span></p>
<textarea id="memory" rows="5"></textarea>
</main>
<div class="save"><div><span id="msg"></span><button class="primary" id="save" disabled>保存</button></div></div>
<script>
// 预置供应商：地址和能力按各家官方文档核对过（2026-10）。模型名变得快，这里只放几个，其余点"获取模型列表"。
// 语音合成要能返回 opus，文档写明支持的只有硅基流动和 OpenAI。
const PRESETS = [
  {id: 'siliconflow', name: '硅基流动', url: 'https://api.siliconflow.cn/v1', caps: '对话 · 识别 · 合成',
   models: ['deepseek-ai/DeepSeek-V4-Flash', 'FunAudioLLM/SenseVoiceSmall', 'FunAudioLLM/CosyVoice2-0.5B'],
   defaults: {chat: 'deepseek-ai/DeepSeek-V4-Flash', asr: 'FunAudioLLM/SenseVoiceSmall', tts: 'FunAudioLLM/CosyVoice2-0.5B'},
   voices: ['anna', 'bella', 'claire', 'diana', 'alex', 'benjamin', 'charles', 'david'].map(v => 'FunAudioLLM/CosyVoice2-0.5B:' + v),
   note: '一个 Key 就能把对话、识别、合成都配齐'},
  {id: 'openai', name: 'OpenAI', url: 'https://api.openai.com/v1', caps: '对话 · 识别 · 合成',
   models: ['gpt-4o-mini-transcribe', 'gpt-4o-mini-tts'], defaults: {asr: 'gpt-4o-mini-transcribe', tts: 'gpt-4o-mini-tts'},
   voices: ['coral', 'alloy', 'marin', 'cedar', 'echo', 'fable', 'nova', 'onyx', 'shimmer'],
   note: '对话模型点"获取模型列表"选；官方说 /audio/speech 的合成模型 2027-01-06 停用'},
  {id: 'deepseek', name: 'DeepSeek', url: 'https://api.deepseek.com', caps: '对话',
   models: ['deepseek-flash'], defaults: {chat: 'deepseek-flash'}, note: '只能对话，识别和合成要另加一家'},
  {id: 'zhipu', name: '智谱', url: 'https://open.bigmodel.cn/api/paas/v4', caps: '对话 · 识别',
   models: ['glm-asr-2512'], defaults: {asr: 'glm-asr-2512'}, note: '合成只出 wav，暂时不能当语音合成；拿不到模型列表就手动添加对话模型'},
  {id: 'bailian', name: '阿里百炼', url: 'https://dashscope.aliyuncs.com/compatible-mode/v1', caps: '对话',
   note: '识别和合成不是 OpenAI 兼容接口，只能当对话用；拿不到模型列表就手动添加，如 qwen3.8-max'},
  {id: 'ark', name: '火山方舟', url: 'https://ark.cn-beijing.volces.com/api/v3', caps: '对话', note: '只能当对话用；模型名到方舟控制台查'},
  {id: 'moonshot', name: 'Kimi', url: 'https://api.moonshot.cn/v1', caps: '对话', note: '只能当对话用；国际站 api.moonshot.ai 的 Key 不通用'},
  {id: 'groq', name: 'Groq', url: 'https://api.groq.com/openai/v1', caps: '对话 · 识别',
   models: ['whisper-large-v3-turbo'], defaults: {asr: 'whisper-large-v3-turbo'}, note: '合成不支持 opus'},
  {id: 'openrouter', name: 'OpenRouter', url: 'https://openrouter.ai/api/v1', caps: '对话 · 识别',
   models: ['openai/whisper-1'], defaults: {asr: 'openai/whisper-1'}, note: '合成只出 mp3 / pcm，暂时不能当语音合成'},
  {id: 'ollama', name: 'Ollama（局域网）', url: 'http://192.168.1.100:11434/v1', caps: '对话 · 识别',
   note: '把地址里的 IP 换成跑 Ollama 那台电脑的局域网 IP，设备访问不到 localhost'},
];

const $ = s => document.querySelector(s);
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
let cfg = {providers: [], roles: {}, limits: {providers: 8, models: 30, memory: 2000}};
let open = null, picker = null;  // 正在编辑的供应商、拉回来的模型列表

// 按模型名猜用途，只用来排序和打标签
const kind = m => /whisper|sensevoice|asr|transcri|paraformer/i.test(m) ? '识别'
  : /tts|speech|cosyvoice|voice/i.test(m) ? '合成'
  : /vl|vision|4o|omni|glm-4v|gemini|-v$/i.test(m) ? '视觉' : '对话';
const msg = (text, cls) => { $('#msg').textContent = text; $('#msg').className = cls || ''; };
const presetOf = p => PRESETS.find(x => x.id === p.preset);
const nextId = () => { for (let i = 1; ; i++) if (!cfg.providers.some(p => p.id === 'p' + i)) return 'p' + i; };
const host = u => u.replace(/^https?:\/\//, '').replace(/\/.*$/, '');

function renderProviders() {
  const box = $('#providers');
  box.innerHTML = cfg.providers.length ? '' : '<p class="hint">还没有供应商。</p>';
  for (const p of cfg.providers) {
    const card = document.createElement('div');
    card.className = 'card';
    const head = document.createElement('div');
    head.className = 'prov-head';
    // 改名称 / 地址 / Key 时只刷新这一行，不重画整张卡片（重画会吞掉正在点的按钮）
    const drawHead = () => {
      const keyTag = p.key ? '<span class="tag ok">新 Key</span>' : p.has_key ? '<span class="tag ok">已填 Key</span>' : '<span class="tag err">没填 Key</span>';
      head.innerHTML = `<div><b>${esc(p.name)}</b><div class="hint">${esc(host(p.url))} · ${p.models.length} 个模型</div></div>${keyTag}`;
    };
    drawHead();
    head.onclick = () => { open = open === p.id ? null : p.id; picker = null; renderProviders(); };
    card.appendChild(head);
    if (open === p.id) card.appendChild(editor(p, () => { drawHead(); renderRoles(); }));
    box.appendChild(card);
  }
  $('#add').disabled = cfg.providers.length >= cfg.limits.providers;
  renderRoles();
}

function editor(p, refresh) {
  const el = document.createElement('div');
  const preset = presetOf(p);
  el.innerHTML = `
    <label>名称<input data-f="name" value="${esc(p.name)}"></label>
    <label>API 地址<input data-f="url" value="${esc(p.url)}" placeholder="https://…/v1"></label>
    <p class="err" data-err hidden></p>
    <p class="err" data-local hidden>设备上的 127.0.0.1 / localhost 指的是设备自己，访问不到你电脑上的服务；要填电脑的局域网 IP，比如 http://192.168.1.100:9880/v1。</p>
    <p class="hint">会请求 <span data-u></span>/chat/completions${preset && preset.note ? '。' + esc(preset.note) : ''}</p>
    <label>Key<input data-f="key" type="password" autocomplete="off" placeholder="${p.has_key ? '已保存，留空不修改' : '填这个供应商的 API Key'}" value="${esc(p.key || '')}"></label>
    <label>模型</label>
    <div class="chips">${p.models.map((m, i) => `<span class="chip"><span title="${esc(m)}">${esc(m)}</span><small class="hint">${kind(m)}</small><button data-del="${i}" title="移除">×</button></span>`).join('') || '<span class="hint">还没有模型，点"获取模型列表"或手动添加</span>'}</div>
    <div class="row"><button data-fetch>获取模型列表</button><input data-manual placeholder="手动输入模型名"><button data-addm>添加</button></div>
    <div data-pick></div>
    <div class="row"><button class="danger" data-remove>删除这个供应商</button></div>`;
  const u = el.querySelector('[data-u]');
  const showUrl = () => {
    u.textContent = (p.url || '').replace(/\/+$/, '') || '…';
    el.querySelector('[data-local]').hidden = !/^https?:\/\/(localhost|127\.|0\.0\.0\.0)/i.test(p.url || '');
  };
  showUrl();
  for (const input of el.querySelectorAll('[data-f]')) {
    input.oninput = () => { p[input.dataset.f] = input.value.trim(); if (input.dataset.f === 'url') showUrl(); refresh(); };
  }
  el.querySelectorAll('[data-del]').forEach(b => b.onclick = () => { p.models.splice(+b.dataset.del, 1); renderProviders(); });
  const manual = el.querySelector('[data-manual]');
  const addManual = () => { const m = manual.value.trim(); if (m) addModel(p, m); };
  el.querySelector('[data-addm]').onclick = addManual;
  manual.onkeydown = e => { if (e.key === 'Enter') addManual(); };
  el.querySelector('[data-remove]').onclick = () => {
    if (!confirm(`删除「${p.name}」？用到它的用途会被清空。`)) return;
    cfg.providers = cfg.providers.filter(x => x !== p);
    for (const r of Object.values(cfg.roles)) if (r.p === p.id) { r.p = ''; r.m = ''; }
    open = null; renderProviders();
  };
  const fetchBtn = el.querySelector('[data-fetch]');
  fetchBtn.onclick = async () => {
    fetchBtn.disabled = true; fetchBtn.textContent = '获取中…';
    try {
      const r = await (await fetch('/models', {method: 'POST', body: JSON.stringify({id: p.id, url: p.url, key: p.key || ''})})).json();
      if (!r.ok) throw new Error(r.error);
      picker = {id: p.id, models: r.models, q: ''};
      if (!r.models.length) msg('这个地址没有返回模型，可以手动添加', 'err');
    } catch (e) { msg('获取模型失败：' + e.message, 'err'); }
    renderProviders();
  };
  if (picker && picker.id === p.id) el.querySelector('[data-pick]').appendChild(pickerView(p));
  if (problem && problem.id === p.id) {
    const err = el.querySelector('[data-err]'), field = el.querySelector(`[data-f=${problem.field}]`);
    err.textContent = problem.text; err.hidden = false;
    field.style.borderColor = 'var(--err)';
    setTimeout(() => { field.scrollIntoView({block: 'center'}); field.focus(); });
  }
  return el;
}

// 保存前先在页面里查一遍：哪个供应商不对，就展开它、标红那一栏
let problem = null;
function check() {
  for (const p of cfg.providers) {
    const bad = !p.name ? {field: 'name', text: '名称要填'}
      : !/^https?:\/\/[^/\s]+/.test(p.url) ? {field: 'url', text: '地址要以 http:// 或 https:// 开头'}
      : null;
    if (bad) return {id: p.id, name: p.name || '这个供应商', ...bad};
  }
  return null;
}

function pickerView(p) {
  const el = document.createElement('div');
  el.innerHTML = `<div class="row"><input placeholder="搜索 ${picker.models.length} 个模型" value="${esc(picker.q)}"></div><div class="picker"></div>`;
  const list = el.querySelector('.picker'), search = el.querySelector('input');
  const draw = () => {
    const q = picker.q.toLowerCase();
    list.innerHTML = '';
    for (const m of picker.models.filter(m => m.toLowerCase().includes(q)).slice(0, 200)) {
      const row = document.createElement('div');
      const on = p.models.includes(m);
      row.className = on ? 'on' : '';
      row.innerHTML = `<span>${esc(m)}</span><span class="hint">${kind(m)} ${on ? '✓' : '+'}</span>`;
      row.onclick = () => { on ? p.models.splice(p.models.indexOf(m), 1) : addModel(p, m, true); renderProviders(); };
      list.appendChild(row);
    }
  };
  search.oninput = () => { picker.q = search.value; draw(); };
  draw();
  return el;
}

function addModel(p, m, quiet) {
  if (p.models.includes(m)) return;
  if (p.models.length >= cfg.limits.models) return msg(`每个供应商最多 ${cfg.limits.models} 个模型`, 'err');
  p.models.push(m);
  if (!quiet) renderProviders();
}

// 四个用途的下拉：按供应商分组，猜得上用途的模型排前面
const ROLE_KIND = {chat: '对话', vision: '视觉', asr: '识别', tts: '合成'};
function renderRoles() {
  for (const role of Object.keys(ROLE_KIND)) {
    const sel = $('#r_' + role), cur = cfg.roles[role] || {p: '', m: ''};
    let html = role === 'vision' ? '<option value="">和对话模型相同</option>' : '<option value="">— 还没选 —</option>';
    for (const p of cfg.providers) {
      const models = [...p.models].sort((a, b) => (kind(b) === ROLE_KIND[role]) - (kind(a) === ROLE_KIND[role]));
      if (models.length) html += `<optgroup label="${esc(p.name)}">` + models.map(m => `<option value="${esc(p.id + '|' + m)}">${esc(m)}</option>`).join('') + '</optgroup>';
    }
    sel.innerHTML = html;
    sel.value = cur.p ? cur.p + '|' + cur.m : '';
    if (sel.value === '' && cur.p) { cur.p = ''; cur.m = ''; }  // 模型被移除了
    sel.onchange = () => {
      const [pid, ...m] = sel.value.split('|');
      cfg.roles[role] = {...cur, p: pid || '', m: m.join('|')};
      if (role === 'tts') syncVoice();
      renderRoles();
    };
  }
  const tts = cfg.providers.find(p => p.id === (cfg.roles.tts || {}).p), preset = tts && presetOf(tts);
  $('#voices').innerHTML = (preset && preset.voices || []).map(v => `<option value="${esc(v)}">`).join('');
  const missing = ['chat', 'asr', 'tts'].filter(r => !(cfg.roles[r] || {}).p).map(r => ({chat: '对话', asr: '语音识别', tts: '语音合成'})[r]);
  $('#missing').hidden = !missing.length;
  $('#missing').textContent = `还没选：${missing.join('、')}。选全之前设备不能对话。`;
}

// 合成换了一家：音色不在这家的列表里就换成它的第一个
function syncVoice() {
  const p = cfg.providers.find(x => x.id === (cfg.roles.tts || {}).p), voices = p && (presetOf(p) || {}).voices;
  if (voices && !voices.includes($('#tts_voice').value)) $('#tts_voice').value = voices[0];
}

$('#add').onclick = () => { $('#presetBox').hidden = !$('#presetBox').hidden; };
function renderPresets() {
  $('#presets').innerHTML = '';
  for (const pr of [...PRESETS, {id: '', name: '自定义', url: '', caps: 'OpenAI 兼容接口'}]) {
    const b = document.createElement('button');
    b.innerHTML = `${esc(pr.name)}<small>${esc(pr.caps || '')}</small>`;
    b.onclick = () => {
      const p = {id: nextId(), name: pr.name === '自定义' ? '自定义' : pr.name, preset: pr.id, url: pr.url, models: [...(pr.models || [])], has_key: false, key: ''};
      cfg.providers.push(p);
      // 这家能做的用途，没选过的就先帮你选上推荐模型
      for (const [role, m] of Object.entries(pr.defaults || {})) {
        if (!(cfg.roles[role] || {}).p) cfg.roles[role] = {...(cfg.roles[role] || {}), p: p.id, m};
      }
      syncVoice();
      open = p.id; picker = null;
      $('#presetBox').hidden = true;
      renderProviders();
    };
    $('#presets').appendChild(b);
  }
}

const memSize = () => $('#memSize').textContent = `（${new TextEncoder().encode($('#memory').value).length} / ${cfg.limits.memory} 字节）`;
$('#memory').oninput = memSize;

$('#save').onclick = async () => {
  // 点了添加却什么都没填的供应商直接丢掉，免得挡住保存
  const blank = p => !p.has_key && !p.key && !p.models.length && (!p.url || p.url === (presetOf(p) || {}).url) && !Object.values(cfg.roles).some(r => r.p === p.id);
  cfg.providers = cfg.providers.filter(p => !blank(p));
  problem = check();
  if (problem) { open = problem.id; picker = null; renderProviders(); msg(`「${problem.name}」${problem.text}`, 'err'); problem = null; return; }
  const ttsProvider = cfg.providers.find(p => p.id === cfg.roles.tts.p), voice = $('#tts_voice').value.trim();
  const voices = (ttsProvider && (presetOf(ttsProvider) || {}).voices) || (voice ? [voice] : []);
  const body = {
    providers: cfg.providers.map(p => ({id: p.id, name: p.name, preset: p.preset, url: p.url, models: p.models, key: p.key || ''})),
    roles: {...cfg.roles, asr: {...cfg.roles.asr, lang: $('#asr_lang').value.trim()}, tts: {...cfg.roles.tts, voice, voices, speak: $('#tts_speak').checked}},
    prompt: $('#prompt').value, memory_md: $('#memory').value,
  };
  $('#save').disabled = true; msg('保存中…');
  try {
    const r = await (await fetch('/save', {method: 'POST', body: JSON.stringify(body)})).json();
    if (!r.ok) throw new Error(r.error);
    msg('已保存，下一轮对话生效。', 'ok');
    await load();
  } catch (e) { msg(e.message, 'err'); }
  $('#save').disabled = false;
};

// 读到当前设置之前不让保存，免得空页面把已有设置冲掉
async function load() {
  const c = await (await fetch('/config.json', {cache: 'no-store'})).json();
  cfg = c;
  for (const r of ['chat', 'vision', 'asr', 'tts']) cfg.roles[r] = cfg.roles[r] || {p: '', m: ''};
  $('#asr_lang').value = c.roles.asr.lang || '';
  $('#tts_voice').value = c.roles.tts.voice || '';
  $('#tts_speak').checked = c.roles.tts.speak !== false;
  $('#prompt').value = c.prompt || '';
  $('#memory').value = c.memory_md || '';
  memSize();
  renderPresets();
  renderProviders();
}
load().then(() => { $('#save').disabled = false; }).catch(() => msg('读取当前设置失败，刷新再试。', 'err'));
</script>
</html>
)HTML";

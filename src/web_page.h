// Single-page web UI, served from flash. Talks to the /api/* endpoints in main.cpp.
#pragma once
#include <Arduino.h>

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lab PSU</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1b1d21;--mute:#6b7280;--line:#e2e4e8;--acc:#2563eb;--on:#16a34a;--off:#9ca3af;--bad:#dc2626}
@media (prefers-color-scheme:dark){:root{--bg:#121417;--card:#1c1f24;--fg:#e8eaed;--mute:#9aa0a6;--line:#2c3036;--acc:#60a5fa;--on:#22c55e;--off:#4b5563;--bad:#f87171}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:18px;margin:0 0 12px;display:flex;justify-content:space-between;align-items:center}
h1 small{font-weight:400;color:var(--mute);font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.meters{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.m b{display:block;font:600 34px/1.1 ui-monospace,monospace;font-variant-numeric:tabular-nums}
.m span{color:var(--mute);font-size:13px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
label{color:var(--mute);font-size:13px;display:block;margin:8px 0 4px}
input[type=number]{width:110px;padding:8px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
input[type=range]{flex:1;min-width:140px}
button{padding:9px 14px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit;cursor:pointer}
button.pri{background:var(--acc);border-color:var(--acc);color:#fff}
#out{width:100%;padding:16px;font-size:20px;font-weight:600;border:0;color:#fff;background:var(--off)}
#out.on{background:var(--on)}
.pdos{display:flex;gap:6px;flex-wrap:wrap}
.trip{display:none;color:var(--bad);font-weight:600}.trip.show{display:flex}
#msg{color:var(--mute);font-size:13px;min-height:1.2em;margin-top:6px}
</style></head><body><main>
<h1>Lab PSU <small id="set">—</small></h1>

<div class="card meters">
  <div class="m"><b id="v">-.---</b><span>Volts</span></div>
  <div class="m"><b id="i">-.---</b><span>Amps</span></div>
  <div class="m"><b id="p">-.--</b><span>Watts</span></div>
  <div class="m"><b id="t">--.-</b><span>°C (board)</span></div>
</div>

<div class="card">
  <div class="row trip" id="trip">Over-current cutoff tripped <button onclick="api('clear')">Reset</button></div>
  <button id="out" onclick="api('output',{on:S.on?0:1})">OUTPUT OFF</button>
  <div id="msg"></div>
</div>

<div class="card" id="ppsCard" hidden>
  <strong>Adjustable (PPS)</strong> <small id="ppsRange" style="color:var(--mute)"></small>
  <label>Voltage (V)</label>
  <div class="row"><input type="range" id="vr" step="0.1"><input type="number" id="vn" step="0.1"></div>
  <label>Current limit (A), enforced by the charger</label>
  <div class="row"><input type="range" id="ir" step="0.05"><input type="number" id="in" step="0.05"></div>
  <div class="row" style="margin-top:10px"><button class="pri" onclick="setPPS()">Apply</button></div>
</div>

<div class="card">
  <strong>Fixed voltages</strong>
  <div class="pdos" id="pdos" style="margin-top:8px"></div>
</div>

<div class="card">
  <strong>Over-current cutoff</strong>
  <label>Output switches off above (A)</label>
  <div class="row"><input type="number" id="ocp" step="0.1" min="0.1" max="7"><button onclick="api('ocp',{ma:Math.round(ocp.value*1000)})">Set</button></div>
</div>
</main>
<script>
let S={},P=[],pps=null;const $=id=>document.getElementById(id);
function pair(r,n){r.oninput=()=>n.value=r.value;n.oninput=()=>r.value=n.value}
pair($('vr'),$('vn'));pair($('ir'),$('in'));
async function api(path,q){
  const u='/api/'+path+(q?'?'+new URLSearchParams(q):'');
  try{const r=await fetch(u,{method:'POST'});const t=await r.text();$('msg').textContent=r.ok?'':t;}
  catch(e){$('msg').textContent='No connection'}
  poll();
}
function setPPS(){api('set',{mode:'pps',mv:Math.round($('vn').value*1000),ma:Math.round($('in').value*1000)})}
async function loadPdos(){
  P=await (await fetch('/api/pdos')).json();
  const box=$('pdos');box.innerHTML='';
  P.forEach(d=>{
    if(d.type=='fixed'){const b=document.createElement('button');
      b.textContent=(d.mv/1000)+' V';b.title='up to '+d.ma/1000+' A';
      b.onclick=()=>api('set',{mode:'fixed',idx:d.idx,ma:d.ma});box.appendChild(b);}
    else if(d.type=='pps')pps=d;
  });
  if(pps){$('ppsCard').hidden=false;
    $('ppsRange').textContent=(pps.min_mv/1000)+'–'+(pps.max_mv/1000)+' V, max '+pps.ma/1000+' A';
    for(const id of['vr','vn']){$(id).min=pps.min_mv/1000;$(id).max=pps.max_mv/1000}
    for(const id of['ir','in']){$(id).min=1;$(id).max=pps.ma/1000}
    $('vr').value=$('vn').value=5;$('ir').value=$('in').value=1;}
}
let first=true;
async function poll(){
  try{S=await (await fetch('/api/status')).json()}catch(e){return}
  $('v').textContent=S.v.toFixed(3);$('i').textContent=S.i.toFixed(3);
  $('p').textContent=S.p.toFixed(2);$('t').textContent=S.t.toFixed(1);
  const o=$('out');o.className=S.on?'on':'';o.textContent=S.on?'OUTPUT ON':'OUTPUT OFF';
  $('trip').classList.toggle('show',S.tripped);
  $('set').textContent=S.mode=='pps'?'PPS '+(S.set_mv/1000).toFixed(1)+' V / '+(S.set_ma/1000).toFixed(2)+' A'
    :S.mode=='fixed'?'Fixed '+(S.set_mv/1000)+' V':'Default 5 V';
  if(first){$('ocp').value=(S.ocp_ma/1000).toFixed(1);
    if(S.mode=='pps'){$('vr').value=$('vn').value=S.set_mv/1000;$('ir').value=$('in').value=S.set_ma/1000}
    first=false}
}
loadPdos().then(poll);setInterval(poll,500);
</script></body></html>
)HTML";

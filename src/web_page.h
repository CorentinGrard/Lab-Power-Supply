// Single-page web UI, served from flash. Talks to the /api/* endpoints in main.cpp.
#pragma once
#include <Arduino.h>

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lab PSU</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1b1d21;--mute:#6b7280;--line:#e2e4e8;--acc:#2563eb;--on:#16a34a;--off:#9ca3af;--bad:#dc2626;--s1:#2a78d6;--s2:#eb6834}
@media (prefers-color-scheme:dark){:root{--bg:#121417;--card:#1c1f24;--fg:#e8eaed;--mute:#9aa0a6;--line:#2c3036;--acc:#60a5fa;--on:#22c55e;--off:#4b5563;--bad:#f87171;--s1:#3987e5;--s2:#d95926}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:18px;margin:0 0 12px;display:flex;justify-content:space-between;align-items:center;gap:8px}
h1 small{font-weight:400;color:var(--mute);font-size:13px;text-align:right}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.meters{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.m b{display:block;font:600 34px/1.1 ui-monospace,monospace;font-variant-numeric:tabular-nums}
.m span{color:var(--mute);font-size:13px}
.m em{display:block;font-style:normal;color:var(--mute);font-size:12px;font-variant-numeric:tabular-nums;min-height:1.3em}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:4px 12px}
label{color:var(--mute);font-size:13px;display:block;margin:8px 0 4px}
input[type=number]{width:110px}
input[type=number],select{padding:8px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
input[type=range]{flex:1;min-width:140px}
button,a.btn{padding:9px 14px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit;cursor:pointer;text-decoration:none}
button.pri{background:var(--acc);border-color:var(--acc);color:#fff}
button.sel{border-color:var(--acc);color:var(--acc)}
#out{width:100%;padding:16px;font-size:20px;font-weight:600;border:0;color:#fff;background:var(--off)}
#out.on{background:var(--on)}
.chips{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px}
.banner{display:none;gap:8px;align-items:center;justify-content:space-between;margin-bottom:10px;font-weight:600}
.banner.show{display:flex}.banner.bad{color:var(--bad)}
.hint{color:var(--mute);font-size:13px}
#msg{position:fixed;left:50%;bottom:16px;transform:translateX(-50%);max-width:calc(100% - 32px);background:var(--fg);color:var(--bg);padding:9px 14px;border-radius:8px;font-size:14px;box-shadow:0 4px 12px #0003;opacity:0;transition:opacity .2s;pointer-events:none}
#msg.show{opacity:1}#msg.bad{background:var(--bad);color:#fff}
button.done{border-color:var(--on);color:var(--on)}button.fail{border-color:var(--bad);color:var(--bad)}
.counts b{font:600 20px/1.2 ui-monospace,monospace;font-variant-numeric:tabular-nums;display:block}
.ch{position:relative;touch-action:pan-y}
.ch h3{font-size:13px;font-weight:600;margin:4px 0 2px;display:flex;align-items:center;gap:6px}
.ch h3 i{display:inline-block;width:12px;height:2px;border-radius:1px}
canvas{display:block;width:100%;height:110px}
#tip{position:absolute;top:0;pointer-events:none;background:var(--card);border:1px solid var(--line);border-radius:8px;padding:6px 8px;font-size:12px;line-height:1.4;white-space:nowrap;display:none;font-variant-numeric:tabular-nums;box-shadow:0 2px 8px #0002}
.tabs button{padding:5px 10px;font-size:13px}
table{width:100%;border-collapse:collapse;margin-top:8px;font-size:14px;font-variant-numeric:tabular-nums}
th{text-align:left;font-weight:400;color:var(--mute);font-size:12px}th,td{padding:5px 6px;border-bottom:1px solid var(--line)}
td.n,th.n{text-align:right}.tag{font-size:11px;color:var(--mute);border:1px solid var(--line);border-radius:4px;padding:0 4px;margin-left:4px}
#tip b{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px}
</style></head><body><main>
<h1>Lab PSU <small id="set">—</small></h1>

<div class="card meters">
  <div class="m"><b id="v">-.---</b><span>Volts</span><em id="vset"></em></div>
  <div class="m"><b id="i">-.---</b><span>Amps</span><em id="iset"></em></div>
  <div class="m"><b id="p">-.--</b><span>Watts</span><em></em></div>
  <div class="m"><b id="t">--.-</b><span>°C (board)</span><em></em></div>
</div>

<div class="card">
  <div class="banner" id="banner"><span id="note"></span><button id="reset" onclick="api('clear')">Reset</button></div>
  <button id="out" onclick="api('output',{on:S.on||S.ramping?0:1})">OUTPUT OFF</button>
  <div id="msg"></div>
</div>

<div class="card ch" id="charts">
  <div class="row" style="justify-content:space-between"><strong>Chart</strong>
    <span class="row tabs" id="viewTabs"><button data-v="live" class="sel">2 min</button><button data-v="m10">10 min</button><button data-v="h1">1 h</button><button data-v="all">All</button></span></div>
  <h3><i style="background:var(--s1)"></i>Voltage (V)</h3><canvas id="cv"></canvas>
  <h3><i style="background:var(--s2)"></i>Current (A)</h3><canvas id="ci"></canvas>
  <div id="tip"></div>
</div>

<div class="card" id="adjCard" hidden>
  <div class="row" style="justify-content:space-between"><strong>Adjustable</strong>
    <span class="row" id="adjTabs"></span></div>
  <div class="hint" id="adjRange"></div>
  <label>Voltage (V)</label>
  <div class="row"><input type="range" id="vr"><input type="number" id="vn"></div>
  <label id="ilabel">Current limit (A)</label>
  <div class="row"><input type="range" id="ir" step="0.05"><input type="number" id="in" step="0.05"></div>
  <div class="row" style="margin-top:10px"><button id="adjBtn" onclick="setAdj(this)">Apply</button></div>
</div>

<div class="card">
  <strong>Fixed voltages</strong>
  <div class="chips" id="pdos"></div>
</div>

<div class="card">
  <strong>Charger &amp; cable</strong>
  <div id="pdSum" style="margin-top:4px"></div>
  <table id="pdTable"></table>
  <div style="margin-top:10px"><span class="hint">Cable</span> <span id="cableRate"></span><br><span id="cableR" class="hint"></span></div>
</div>

<div class="card">
  <strong>Protection</strong>
  <div class="grid">
    <div><label>Current cutoff (A)</label><input type="number" id="ocp" step="0.1" min="0.1" max="7"></div>
    <div><label>Voltage cutoff (V, 0 = off)</label><input type="number" id="ovp" step="0.1" min="0" max="50"></div>
    <div><label>Power cutoff (W, 0 = off)</label><input type="number" id="opp" step="1" min="0" max="250"></div>
    <div><label>Soft start (s, 0 = off)</label><input type="number" id="ramp" step="0.1" min="0" max="10"></div>
  </div>
  <div class="row" style="margin-top:10px"><button id="protBtn" onclick="saveProt(this)">Save</button>
    <span class="hint">Cutoffs switch the output off. Soft start ramps PPS/AVS up from the lowest voltage.</span></div>
</div>

<div class="card">
  <div class="row" style="justify-content:space-between"><strong>Counters</strong><button onclick="api('counters/reset',null,this,'Reset')">Reset</button></div>
  <div class="grid counts" style="margin-top:8px;grid-template-columns:1fr 1fr 1fr">
    <div><b id="ont">0:00:00</b><span class="hint">on-time</span></div>
    <div><b id="mah">0</b><span class="hint">mAh</span></div>
    <div><b id="wh">0.000</b><span class="hint">Wh</span></div>
  </div>
  <label>Switch the output off after (0 = no limit)</label>
  <div class="row"><input type="number" id="offm" min="0" step="1"><span class="hint">min</span>
    <input type="number" id="offmah" min="0" step="10"><span class="hint">mAh</span></div>
  <label>End of charge: switch off when the current stays below (0 = off)</label>
  <div class="row"><input type="number" id="endma" min="0" max="5000" step="10"><span class="hint">mA for</span>
    <input type="number" id="ends" min="1" max="3600" step="1"><span class="hint">s</span></div>
  <div class="row" style="margin-top:10px"><button id="timerBtn" onclick="saveTimer(this)">Set</button><span class="hint" id="lowInfo"></span></div>
</div>

<div class="card">
  <strong>Log</strong> <span class="hint" id="logInfo"></span>
  <div class="row" style="margin-top:8px">
    <select id="logInt" onchange="api('log',{interval_s:this.value},null,'Log interval changed, log cleared')">
      <option value="1">every 1 s (50 min)</option><option value="5">every 5 s (4 h)</option>
      <option value="10">every 10 s (8 h)</option><option value="60">every 1 min (2 days)</option>
    </select>
    <a class="btn" href="/api/log.csv">Download CSV</a></div>
  <div class="hint" style="margin-top:6px">Kept in memory, lost on reboot. Changing the interval clears it.</div>
</div>
</main>
<script>
let S={},P=[],adj={},adjMode='',gen=-1,first=true,floor=0,toastT;
const $=id=>document.getElementById(id);
const fv=mv=>+(mv/1000).toFixed(2)+' V',fa=ma=>ma?+(ma/1000).toFixed(2)+' A':'max A';
function pair(r,n){r.oninput=()=>n.value=r.value;n.oninput=()=>r.value=n.value}
pair($('vr'),$('vn'));pair($('ir'),$('in'));
function toast(text,bad){const m=$('msg');m.textContent=text;m.className='show'+(bad?' bad':'');
  clearTimeout(toastT);toastT=setTimeout(()=>m.className=bad?'bad':'',bad?5000:2000)}
// POST to the API. Errors show as a toast; b (a button) briefly shows the outcome, ok is the success text.
async function api(path,q,b,ok){
  const u='/api/'+path+(q?'?'+new URLSearchParams(q):'');let good=false,t;
  try{const r=await fetch(u,{method:'POST'});t=await r.text();good=r.ok}catch(e){t='No connection'}
  if(!good)toast(t,true);else if(ok&&!b)toast(ok);
  if(b){const l=b.dataset.l||(b.dataset.l=b.textContent);b.textContent=good?'✓ '+(ok||'Done'):'✗ Failed';
    b.classList.add(good?'done':'fail');setTimeout(()=>{b.textContent=l;b.classList.remove('done','fail')},1500)}
  await poll();return good;
}
function btn(text,title,fn){const b=document.createElement('button');b.textContent=text;if(title)b.title=title;b.onclick=fn;return b}
function setAdj(b){api('set',{mode:adjMode,mv:Math.round($('vn').value*1000),ma:Math.round($('in').value*1000)},b,'Applied')}
function saveProt(b){api('protect',{ocp_ma:Math.round(ocp.value*1000),ovp_mv:Math.round(ovp.value*1000),
  opp_mw:Math.round(opp.value*1000),ramp_ms:Math.round(ramp.value*1000)},b,'Saved')}
function saveTimer(b){api('timer',{off_s:Math.round(offm.value*60),off_mah:Math.round(offmah.value),
  end_ma:Math.round(endma.value),end_s:Math.round(ends.value)},b,'Saved')}
// Highlight a button while its inputs differ from what the supply has
const mk=v=>Math.round(v*1000);
function markEdits(){
  $('protBtn').classList.toggle('pri',mk(ocp.value)!=S.ocp_ma||mk(ovp.value)!=S.ovp_mv||mk(opp.value)!=S.opp_mw||mk(ramp.value)!=S.ramp_ms);
  $('timerBtn').classList.toggle('pri',Math.round(offm.value*60)!=S.off_s||Math.round(offmah.value)!=S.off_mah
    ||Math.round(endma.value)!=S.end_ma||Math.round(ends.value)!=S.end_s);
  $('adjBtn').classList.toggle('pri',adjMode!=S.mode||mk($('vn').value)!=S.set_mv||(adjMode=='pps'&&mk($('in').value)!=S.set_ma));
}
document.addEventListener('input',markEdits);
function setAdjMode(m){
  adjMode=m;const d=adj[m],lo=Math.max(d.min_mv,floor)/1000;
  [...$('adjTabs').children].forEach(b=>b.classList.toggle('sel',b.dataset.m==m));
  $('adjRange').textContent=m.toUpperCase()+' '+lo+'–'+(d.max_mv/1000)+' V, max '+d.ma/1000+' A';
  const st=m=='avs'?0.2:0.1;
  for(const id of['vr','vn']){$(id).min=lo;$(id).max=d.max_mv/1000;$(id).step=st}
  for(const id of['ir','in']){$(id).min=1;$(id).max=d.ma/1000}
  $('ilabel').textContent=m=='pps'?'Current limit (A), enforced by the charger':'Current (A), use the cutoff to limit it';
  const cur=S.mode==m;
  $('vr').value=$('vn').value=cur?S.set_mv/1000:Math.max(lo,5);
  $('ir').value=$('in').value=cur?S.set_ma/1000:Math.min(1,d.ma/1000);
}
async function loadProfiles(){
  const r=await (await fetch('/api/pdos')).json();P=r.pdos;floor=r.floor_mv||0;
  const box=$('pdos');box.innerHTML='';adj={};
  P.forEach(d=>{
    if(d.type=='fixed')box.appendChild(Object.assign(btn(fv(d.mv),'up to '+d.ma/1000+' A',()=>api('set',{mode:'fixed',idx:d.idx,ma:d.ma})),{mv:d.mv}));
    else if(!adj[d.type])adj[d.type]=d;
  });
  if(!box.children.length)box.innerHTML='<span class="hint">No charger profiles yet</span>';
  // Charger & cable report
  let maxW=0,five=false,epr=false,rows='<tr><th>#</th><th>Type</th><th class="n">Voltage</th><th class="n">Current</th><th class="n">Power</th></tr>';
  P.forEach(d=>{const w=d.max_mv*d.ma/1e6;maxW=Math.max(maxW,w);five||=d.ma>=5000;epr||=d.idx>=8;
    rows+='<tr><td>'+d.idx+'</td><td>'+d.type.toUpperCase()+(d.idx>=8?'<span class="tag">EPR</span>':'')+'</td><td class="n">'
      +(d.type=='fixed'?fv(d.mv):(d.min_mv/1000)+'–'+fv(d.max_mv))+'</td><td class="n">'+(d.ma/1000).toFixed(2)+' A</td><td class="n">'+w.toFixed(0)+' W</td></tr>'});
  $('pdTable').innerHTML=P.length?rows:'';
  const has=t=>P.some(d=>d.type==t);
  $('pdSum').textContent=P.length?Math.round(maxW)+' W max · PPS '+(has('pps')?'yes':'no')+' · AVS '+(has('avs')?'yes':'no')+' · EPR '+(epr?'yes':'no')
    :'No USB PD profiles: a legacy (non-PD) charger, or still negotiating';
  $('cableRate').textContent=!P.length?'—':epr?'EPR rated (50 V, 5 A)':five?'5 A rated (e-marked)':'3 A, or the charger is limited to 3 A';
  const tabs=$('adjTabs');tabs.innerHTML='';const ms=Object.keys(adj);
  $('adjCard').hidden=!ms.length;
  if(ms.length>1)ms.forEach(m=>{const b=btn(m.toUpperCase(),'',()=>setAdjMode(m));b.dataset.m=m;tabs.appendChild(b)});
  if(ms.length)setAdjMode(adj[S.mode]?S.mode:ms[0]);
}
const hms=s=>Math.floor(s/3600)+':'+String(Math.floor(s/60)%60).padStart(2,'0')+':'+String(s%60).padStart(2,'0');
async function poll(){
  try{S=await (await fetch('/api/status')).json()}catch(e){return}
  $('v').textContent=S.v.toFixed(3);$('i').textContent=S.i.toFixed(3);
  $('p').textContent=S.p.toFixed(2);$('t').textContent=S.t.toFixed(1);
  const dv=S.v-S.set_mv/1000;
  $('vset').textContent='set '+(S.set_mv/1000).toFixed(2)+' V'+(S.on&&!S.ramping?' · Δ '+(dv>=0?'+':'−')+Math.abs(dv).toFixed(2):'');
  $('iset').textContent=S.mode=='pps'?'limit '+(S.set_ma/1000).toFixed(2)+' A':'cutoff '+(S.ocp_ma/1000).toFixed(2)+' A';
  const o=$('out');o.className=S.on?'on':'';
  o.textContent=S.on?(S.ramping?'OUTPUT ON · ramping':'OUTPUT ON'):S.ramping?'STARTING…':'OUTPUT OFF';
  $('banner').className='banner'+(S.note?' show':'')+(S.trip?' bad':'');
  $('note').textContent=S.note;$('reset').hidden=!S.trip;
  $('set').textContent=(S.mode=='default'?'Default':S.mode.toUpperCase())+' '+fv(S.set_mv)+(S.mode=='pps'?' / '+fa(S.set_ma):'');
  $('ont').textContent=hms(S.on_s);$('mah').textContent=S.mah.toFixed(1);$('wh').textContent=S.wh.toFixed(3);
  const span=S.log_n*S.log_s;
  $('logInfo').textContent=S.log_n+' samples, '+(span<3600?Math.round(span/60)+' min':(span/3600).toFixed(1)+' h');
  if(first){first=false;
    ocp.value=S.ocp_ma/1000;ovp.value=S.ovp_mv/1000;opp.value=S.opp_mw/1000;ramp.value=S.ramp_ms/1000;
    offm.value=S.off_s/60;offmah.value=S.off_mah;endma.value=S.end_ma;ends.value=S.end_s;$('logInt').value=S.log_s}
  $('lowInfo').textContent=S.end_ma&&S.on&&S.low_s?'below '+S.end_ma+' mA for '+S.low_s+' s':'';
  $('cableR').textContent=S.cable_mohm>=0?'≈ '+S.cable_mohm+' mΩ (cable + connectors, measured at '+S.cable_a.toFixed(2)+' A)'
    :'Resistance: draw more than 0.3 A to measure it';
  if(S.gen!=gen){gen=S.gen;await loadProfiles()}
  [...$('pdos').children].forEach(b=>b.classList.toggle('sel',S.mode=='fixed'&&b.mv==S.set_mv));
  markEdits();
  H.push({t:Date.now(),v:S.v,i:S.i});while(H.length&&H[0].t<Date.now()-LIVE)H.shift();
  updateLog();drawAll();
}
// Charts: two small multiples sharing the time axis.
// "2 min" uses the live readings; longer views use the log, fetched incrementally.
const H=[],LIVE=120000,VIEWS={m10:600,h1:3600,all:0};let hoverX=null,view='live',LG=[],lastT=-1,logAt=0,logBusy=false;
const css=n=>getComputedStyle(document.body).getPropertyValue(n).trim();
const L=40,R=8,T=6,B=16;
const dur=ms=>{const s=Math.round(ms/1000);return s<120?s+' s':s<7200?Math.round(s/60)+' min':(s/3600).toFixed(1).replace('.0','')+' h'};
async function fetchLog(){
  if(logBusy)return;logBusy=true;logAt=Date.now();
  try{const t=await (await fetch('/api/log.csv?since='+lastT)).text();
    t.split('\n').slice(1).forEach(l=>{const f=l.split(',');if(f.length<4)return;LG.push({t:f[0]*1000,v:+f[2],i:+f[3]});lastT=+f[0]});
    if(LG.length>3000)LG.splice(0,LG.length-3000)}catch(e){}
  logBusy=false;drawAll();
}
function series(){
  if(view=='live')return{pts:H,now:Date.now(),win:LIVE,gap:3000};
  const now=(S.up_s||0)*1000,win=VIEWS[view]*1000||Math.max(60000,now-(LG.length?LG[0].t:now));
  return{pts:LG.filter(p=>p.t>=now-win),now,win,gap:(S.log_s||1)*2500};
}
function drawChart(cv,key,color,minSpan,sr,hi){
  const dpr=devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight,{pts,now,win,gap}=sr;
  if(cv.width!=Math.round(w*dpr)){cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr)}
  const c=cv.getContext('2d');c.setTransform(dpr,0,0,dpr,0,0);c.clearRect(0,0,w,h);
  const pw=w-L-R,ph=h-T-B,x=t=>L+pw*(1-(now-t)/win);
  let lo=Infinity,up=-Infinity;pts.forEach(s=>{lo=Math.min(lo,s[key]);up=Math.max(up,s[key])});
  if(!pts.length){lo=0;up=minSpan}
  const dmin=lo;if(up-lo<minSpan){const m=(up+lo)/2;lo=m-minSpan/2;up=m+minSpan/2}
  const pd=(up-lo)*.1;lo-=pd;up+=pd;if(dmin>=0&&lo<0){up-=lo;lo=0}
  const y=v=>T+ph*(1-(v-lo)/(up-lo)),sp=up-lo,dec=sp<1?2:sp<10?1:0;
  c.font='11px system-ui,sans-serif';c.fillStyle=css('--mute');c.strokeStyle=css('--line');c.lineWidth=1;
  c.textAlign='right';c.textBaseline='middle';
  for(let k=0;k<3;k++){const v=lo+sp*k/2,yy=Math.round(y(v))+.5;c.beginPath();c.moveTo(L,yy);c.lineTo(w-R,yy);c.stroke();c.fillText(v.toFixed(dec),L-6,yy)}
  c.textBaseline='alphabetic';[['−'+dur(win),0,'left'],['−'+dur(win/2),.5,'center'],['now',1,'right']].forEach(([s,f,a])=>{c.textAlign=a;c.fillText(s,L+pw*f,h-3)});
  c.strokeStyle=color;c.lineWidth=2;c.lineJoin=c.lineCap='round';c.beginPath();
  pts.forEach((s,k)=>{const X=x(s.t),Y=y(s[key]);(k&&s.t-pts[k-1].t<gap)?c.lineTo(X,Y):c.moveTo(X,Y)});c.stroke();
  if(hi){const X=x(hi.t),Y=y(hi[key]);
    c.strokeStyle=css('--mute');c.lineWidth=1;c.beginPath();c.moveTo(Math.round(X)+.5,T);c.lineTo(Math.round(X)+.5,T+ph);c.stroke();
    c.beginPath();c.arc(X,Y,4,0,7);c.fillStyle=color;c.fill();c.strokeStyle=css('--card');c.lineWidth=2;c.stroke()}
}
function drawAll(){
  let hi=null;const cv=$('cv'),tip=$('tip'),sr=series();
  if(hoverX!=null&&sr.pts.length){const pw=cv.clientWidth-L-R,t=sr.now-sr.win*(1-(hoverX-L)/pw);
    hi=sr.pts.reduce((a,b)=>Math.abs(b.t-t)<Math.abs(a.t-t)?b:a)}
  drawChart(cv,'v',css('--s1'),0.1,sr,hi);drawChart($('ci'),'i',css('--s2'),0.05,sr,hi);
  if(!hi){tip.style.display='none';return}
  const ago=sr.now-hi.t;
  tip.innerHTML=(ago<500?'now':'−'+(ago<120000?Math.round(ago/1000)+' s':Math.floor(ago/60000)+' min '+Math.round(ago/1000)%60+' s'))
    +'<br><b style="background:var(--s1)"></b>'+hi.v.toFixed(3)+' V<br><b style="background:var(--s2)"></b>'+hi.i.toFixed(3)+' A';
  tip.style.display='block';const box=$('charts'),left=cv.offsetLeft+hoverX;
  tip.style.top=(cv.offsetTop+4)+'px';tip.style.left=Math.min(left+12,box.clientWidth-tip.offsetWidth-4)+'px';
  if(left+12+tip.offsetWidth>box.clientWidth-4)tip.style.left=Math.max(4,left-tip.offsetWidth-12)+'px';
}
// Called from poll(): keep the log copy current, start over after a reboot or a log reset
let logS=0,lastUp=0;
function updateLog(){
  if(S.log_s!=logS||S.up_s<lastUp||S.log_n<LG.length){LG=[];lastT=-1;logS=S.log_s}
  lastUp=S.up_s;
  if(view!='live'&&Date.now()-logAt>5000)fetchLog();
}
$('viewTabs').addEventListener('click',e=>{const v=e.target.dataset.v;if(!v)return;view=v;
  [...$('viewTabs').children].forEach(b=>b.classList.toggle('sel',b.dataset.v==v));
  if(v!='live')fetchLog();drawAll()});
const chartsEl=$('charts');
chartsEl.addEventListener('pointermove',e=>{const r=$('cv').getBoundingClientRect(),X=e.clientX-r.left;hoverX=X>=L&&X<=r.width-R&&e.clientY>r.top?X:null;drawAll()});
chartsEl.addEventListener('pointerleave',()=>{hoverX=null;drawAll()});
addEventListener('resize',drawAll);
poll();setInterval(poll,500);
</script></body></html>
)HTML";

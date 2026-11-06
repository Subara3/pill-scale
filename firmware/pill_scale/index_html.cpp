#include <Arduino.h>

// スマホのブラウザで開く画面。白とグレーだけのそっけない見た目にしている。
extern const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>はかり</title>
<style>
body{font-family:system-ui,-apple-system,"Hiragino Sans","Noto Sans JP",sans-serif;margin:0;background:#f4f4f4;color:#222}
main{max-width:520px;margin:0 auto;padding:12px}
section{background:#fff;border:1px solid #ddd;border-radius:6px;padding:12px;margin-bottom:10px}
h2{font-size:14px;margin:0 0 8px;color:#555;font-weight:600}
.count{font-size:64px;font-weight:700;line-height:1.1;text-align:center}
.count small{font-size:20px;font-weight:400;margin-left:4px}
.weight{text-align:center;font-size:20px;color:#444}
.status{text-align:center;font-size:13px;color:#777;margin-top:4px}
.warn{color:#b00}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin:6px 0}
button{font-size:15px;padding:8px 12px;border:1px solid #bbb;background:#fafafa;border-radius:4px}
button.main{background:#333;color:#fff;border-color:#333}
input,select{font-size:15px;padding:7px;border:1px solid #bbb;border-radius:4px}
input[type=number]{width:80px}
#msg{min-height:1.4em;font-size:13px;color:#333}
canvas{width:100%;height:120px;border:1px solid #eee}
table{font-size:12px;color:#666;border-collapse:collapse}
td{padding:1px 8px 1px 0}
</style></head><body><main>

<section>
  <div class="count" id="count">-</div>
  <div class="weight" id="weight">- g</div>
  <div class="status" id="status">接続中</div>
</section>

<section>
  <div class="row">
    <button class="main" onclick="post('/api/tare')">ゼロに合わせる</button>
    <button onclick="post('/api/log')">今の数を記録</button>
  </div>
  <div class="row">
    <select id="item" onchange="post('/api/select',{i:this.value})"></select>
    <button onclick="del()">この品目を消す</button>
  </div>
  <div id="msg"></div>
</section>

<section>
  <h2>飲んだ記録（瓶を置きっぱなしにしたとき）</h2>
  <div class="row" id="today">今日減った粒: -</div>
  <table id="ev"></table>
  <div class="row"><a href="/take.csv">減った記録をCSVで取り出す</a></div>
</section>

<section>
  <h2>1個の重さを覚える</h2>
  <div class="row">
    <input id="lname" placeholder="名前（例: 胃薬）">
    <input id="ln" type="number" value="10" min="1">個
    <button onclick="post('/api/learn',{name:val('lname'),n:val('ln')})">のせた数で覚える</button>
  </div>
  <div class="row">
    選んでいる品目の1個を
    <input id="ug" type="number" step="0.001" value="0.286">g に
    <button onclick="post('/api/unit',{g:val('ug')})">直す</button>
  </div>
</section>

<section>
  <h2>ずれの記録（直近10分、1秒ごと）</h2>
  <canvas id="cv" width="500" height="120"></canvas>
</section>

<section>
  <h2>設定</h2>
  <div class="row">
    ロードセル
    <select id="slot" onchange="post('/api/slot',{s:this.value})">
      <option value="0">小（300g）</option><option value="1">大（10kg）</option>
    </select>
  </div>
  <div class="row">
    <label><input type="checkbox" id="az" onchange="post('/api/autozero',{on:this.checked?1:0})"> 空のとき自動でゼロを合わせ直す</label>
  </div>
  <div class="row">
    校正: 空でゼロに合わせてから
    <input id="calg" type="number" value="10" step="0.01">g をのせて
    <button onclick="post('/api/cal',{g:val('calg')})">校正</button>
  </div>
  <div class="status">1円玉は1枚1.00g。10枚で10g。</div>
  <div class="row"><a href="/log.csv">記録をCSVで取り出す</a></div>
  <table id="dbg"></table>
</section>

</main>
<script>
const $=id=>document.getElementById(id);
const val=id=>$(id).value;
let itemsKey='';
async function post(path,params={}){
  const body=new URLSearchParams(params);
  try{
    const r=await fetch(path,{method:'POST',body});
    const j=await r.json();
    $('msg').textContent=j.msg||'';
    $('msg').className=j.ok?'':'warn';
  }catch(e){$('msg').textContent='送れませんでした';$('msg').className='warn';}
  tick();
}
function del(){
  const i=val('item');
  if(i<0)return;
  post('/api/delete',{i});
}
function fmt(w,slot){return slot==0?w.toFixed(3):w.toFixed(1);}
async function tick(){
  try{
    const s=await (await fetch('/api/state')).json();
    if(!s.sensorOk){$('status').textContent='ユニットが読めません。ケーブルを確かめてください';$('status').className='status warn';}
    else if(!s.calibrated){$('status').textContent='まだ校正していません（設定から）';$('status').className='status warn';}
    else{$('status').textContent=(s.stable?'安定':'ゆれている')+' ・ ±'+fmt(s.sd,s.slot)+' g';$('status').className='status';}
    $('weight').textContent=fmt(s.weight,s.slot)+' g';
    if(s.item>=0&&s.count!==undefined){
      const name=s.items[s.item].name;
      $('count').innerHTML=s.count+'<small>'+(s.slot==1?'冊':'個')+'</small>';
      $('count').title=name;
      if(s.frac>0.25){$('status').textContent+=' ・ 数があいまい（ずれ '+s.frac+'）';$('status').className='status warn';}
    }else{$('count').textContent='-';}
    const key=JSON.stringify(s.items)+s.item;
    if(key!==itemsKey){
      itemsKey=key;
      let h='<option value="-1">品目を選ぶ</option>';
      s.items.forEach((it,i)=>{h+='<option value="'+i+'"'+(i==s.item?' selected':'')+'>'+it.name.replace(/</g,'&lt;')+'（1個 '+it.unit.toFixed(4)+'g）</option>';});
      $('item').innerHTML=h;
    }
    $('today').textContent='今日減った粒: '+(s.todayTaken??'-')+'（瓶の重さ '+(s.baseW===null?'-':s.baseW.toFixed(3)+' g')+'）';
    $('ev').innerHTML=(s.events||[]).map(e=>'<tr><td>'+e.t+'</td><td>'+(e.n>0?e.n+'粒減った':e.n<0?(-e.n)+'粒増えた':'あいまい（'+e.x+'粒分）')+'</td></tr>').join('');
    $('slot').value=s.slot;
    $('az').checked=s.autoZero;
    $('dbg').innerHTML='<tr><td>生の値</td><td>'+s.raw+'</td></tr><tr><td>平均</td><td>'+s.meanRaw+'</td></tr><tr><td>ばらつき</td><td>'+s.sdRaw+'</td></tr><tr><td>1gあたり</td><td>'+s.countsPerGram+'</td></tr><tr><td>読み取り失敗</td><td>'+s.errCount+'</td></tr><tr><td>時刻</td><td>'+s.time+'</td></tr>';
  }catch(e){$('status').textContent='本体とつながりません';$('status').className='status warn';}
}
async function graph(){
  try{
    const h=await (await fetch('/api/history')).json();
    const c=$('cv'),x=c.getContext('2d');
    x.clearRect(0,0,c.width,c.height);
    if(h.length<2)return;
    let lo=Math.min(...h),hi=Math.max(...h);
    if(hi-lo<0.01){const m=(hi+lo)/2;lo=m-0.005;hi=m+0.005;}
    x.strokeStyle='#888';x.beginPath();
    h.forEach((v,i)=>{const px=i*(c.width-1)/(h.length-1),py=c.height-4-(v-lo)/(hi-lo)*(c.height-8);i?x.lineTo(px,py):x.moveTo(px,py);});
    x.stroke();
    x.fillStyle='#666';x.font='11px sans-serif';
    x.fillText(hi.toFixed(3)+' g',4,12);x.fillText(lo.toFixed(3)+' g',4,c.height-4);
  }catch(e){}
}
setInterval(tick,500);setInterval(graph,5000);tick();graph();
</script></body></html>
)HTML";

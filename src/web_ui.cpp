#include "web_ui.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

static std::string jsonEscape(const std::string& s) {
    std::ostringstream out;
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

static std::string layoutJson(const UiLayout& l) {
    std::ostringstream out;
    out << "{\"rows\":" << l.rows
        << ",\"cols\":" << l.cols
        << ",\"flag\":{\"r\":" << l.flagR << ",\"c\":" << l.flagC << "}"
        << ",\"walls\":[";
    for (size_t i = 0; i < l.walls.size(); ++i) {
        if (i) out << ',';
        out << "[" << l.walls[i].r << ',' << l.walls[i].c << "]";
    }
    out << "]}";
    return out.str();
}

static void writeAgentJson(std::ostringstream& out, const UiAgentView& a) {
    out << "{\"id\":" << a.id
        << ",\"role\":" << a.role
        << ",\"name\":\"" << jsonEscape(a.name) << "\""
        << ",\"r\":" << a.r
        << ",\"c\":" << a.c
        << ",\"steps\":" << a.steps
        << ",\"frozen\":" << (a.frozen ? "true" : "false")
        << ",\"canShoot\":" << (a.canShoot ? "true" : "false")
        << ",\"freezeRemainingMs\":" << a.freezeRemainingMs
        << ",\"cooldownRemainingMs\":" << a.cooldownRemainingMs
        << '}';
}

static std::string stateJson(const UiState& s) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{\"rows\":" << s.rows
        << ",\"cols\":" << s.cols
        << ",\"totalAgents\":" << s.totalAgents
        << ",\"displayedAgents\":" << s.displayedAgents
        << ",\"totalSteps\":" << s.totalSteps
        << ",\"frozenAgents\":" << s.frozenAgents
        << ",\"delayMs\":" << s.delayMs
        << ",\"winner\":" << s.winner
        << ",\"paused\":" << (s.paused ? "true" : "false")
        << ",\"gameOver\":" << (s.gameOver ? "true" : "false")
        << ",\"elapsedSec\":" << s.elapsedSec
        << ",\"agents\":[";
    for (size_t i = 0; i < s.agents.size(); ++i) {
        if (i) out << ',';
        writeAgentJson(out, s.agents[i]);
    }
    out << "]";
    out << ",\"selected\":";
    if (s.hasSelected) writeAgentJson(out, s.selected);
    else out << "null";
    out << '}';
    return out.str();
}

static std::map<std::string, std::string> parseQuery(const std::string& query) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string part = query.substr(pos, amp - pos);
        size_t eq = part.find('=');
        if (eq == std::string::npos) out[part] = "";
        else out[part.substr(0, eq)] = part.substr(eq + 1);
        pos = amp + 1;
    }
    return out;
}

static int intParam(const std::map<std::string, std::string>& q,
                    const std::string& key,
                    int fallback) {
    auto it = q.find(key);
    if (it == q.end()) return fallback;
    try { return std::stoi(it->second); }
    catch (...) { return fallback; }
}

static std::string httpResponse(const std::string& body,
                                const std::string& contentType = "application/json; charset=utf-8",
                                const std::string& status = "200 OK") {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "\r\n"
        << body;
    return out.str();
}

static const char* kDashboardHtml = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlagChase — Concurrent 2D Agent Simulation</title>
<style>
:root{--bg:#08111d;--panel:#101c2b;--panel2:#0c1725;--border:#294057;--text:#e6edf7;--muted:#8fa3b8;--green:#35d07f;--blue:#46a7ff;--red:#ff5b4d;--orange:#ff9d3d;--yellow:#ffd95a;--purple:#b47cff}
*{box-sizing:border-box}body{margin:0;background:linear-gradient(180deg,#08111d,#070d16);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:14px;overflow-x:hidden}.top{height:56px;display:flex;align-items:center;padding:0 18px;border-bottom:1px solid #203348;background:#09131f;position:sticky;top:0;z-index:10}.brand{font-size:18px;font-weight:750;letter-spacing:.1px}.brand:before{content:"▲";color:var(--red);margin-right:10px}.topstats{margin-left:auto;display:flex;gap:28px;align-items:center;color:#cbd5e1}.running{color:var(--green)}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:currentColor;margin-right:7px}.app{display:grid;grid-template-columns:300px minmax(520px,1fr) 310px;grid-template-rows:minmax(520px,calc(100vh - 285px)) 220px;gap:10px;padding:10px;min-height:calc(100vh - 56px)}.panel{background:linear-gradient(180deg,var(--panel),var(--panel2));border:1px solid var(--border);border-radius:6px;overflow:hidden;box-shadow:0 8px 28px rgba(0,0,0,.16)}.ptitle{font-size:15px;font-weight:700;padding:10px 12px;border-bottom:1px solid #293c50;background:rgba(255,255,255,.015)}.pbody{padding:12px}.left{display:flex;flex-direction:column;gap:10px;min-height:0}.right{display:flex;flex-direction:column;gap:10px;min-height:0}.left .panel:last-child,.right .panel:last-child{flex:1}.view{position:relative;display:flex;flex-direction:column;min-height:0}.canvaswrap{position:relative;flex:1;min-height:0;background:#050b12;overflow:hidden}.canvaswrap canvas{width:100%;height:100%;display:block;cursor:crosshair}.overlay{position:absolute;right:12px;top:12px;background:rgba(8,17,29,.91);border:1px solid #344b61;border-radius:5px;padding:8px 10px;line-height:1.65;pointer-events:none}.row{display:flex;align-items:center;gap:8px;margin:9px 0}.row label{color:#d3dfeb;flex:1}.value{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;color:#fff}.btn{border:1px solid transparent;border-radius:4px;padding:9px 12px;color:#fff;background:#20344b;font-weight:650;cursor:pointer}.btn:hover{filter:brightness(1.12)}.btn.green{background:#1ba664}.btn.blue{background:#1976c9}.btn.red{background:#bc3735}.btn.orange{background:#c66a18}.btn.secondary{border-color:#425971;background:#132235}.controls{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px;margin-bottom:14px}input[type=range]{width:100%;accent-color:var(--blue)}input[type=number]{width:84px;background:#132235;border:1px solid #38516b;border-radius:4px;color:#fff;padding:7px}select{background:#132235;border:1px solid #38516b;color:#fff;border-radius:4px;padding:7px}.check{display:flex;gap:8px;align-items:center;margin:10px 0;color:#ccd8e4}.legend{display:grid;gap:9px}.legend span{display:flex;align-items:center;gap:9px}.mark{width:14px;height:14px;display:inline-block}.circle{border-radius:50%;background:var(--blue)}.square{background:var(--orange)}.tri{width:0;height:0;border-left:8px solid transparent;border-right:8px solid transparent;border-bottom:15px solid var(--red)}.flag{color:var(--yellow);font-size:18px}.wall{background:#73808c}.stats{display:grid;grid-template-columns:1fr auto;gap:7px 12px}.stats .k{color:#b6c5d3}.stats .v{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}.badge{display:inline-block;border:1px solid #46617c;border-radius:999px;padding:3px 8px;font-size:12px}.badge.frozen{border-color:#63b6ff;color:#86cbff}.agent-actions{display:grid;grid-template-columns:1fr 1fr;gap:7px;margin-top:12px}.spatial{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;color:#a9bdd1;line-height:1.7}.bottom{grid-column:1 / span 2;display:grid;grid-template-columns:1fr 1fr;gap:10px;min-height:0}.chart{padding:8px;height:174px}.chart canvas{width:100%;height:100%}.notes{grid-column:3;grid-row:2}.note{color:#aebfce;line-height:1.55}.note strong{color:#fff}.footer{padding:8px 12px;border-top:1px solid #24384c;color:#8297aa;font-size:12px}.small{font-size:12px;color:var(--muted)}@media(max-width:1200px){.app{grid-template-columns:260px minmax(480px,1fr);grid-template-rows:auto 560px auto auto}.right{grid-column:1 / span 2;display:grid;grid-template-columns:1fr 1fr}.notes{grid-column:1 / span 2;grid-row:auto}.bottom{grid-column:1 / span 2}.view{grid-column:2;grid-row:1 / span 2}}@media(max-width:850px){.app{display:block}.panel,.left,.right,.view,.bottom,.notes{margin-bottom:10px}.view{height:520px}.right,.bottom{display:block}.bottom .panel{height:220px}}
</style>
</head>
<body>
<header class="top"><div class="brand">FlagChase — Concurrent 2D Agent Simulation</div><div class="topstats"><span id="runState" class="running"><i class="dot"></i>Running</span><span>Steps: <b id="topSteps">0</b></span><span>Time: <b id="topTime">00:00:00</b></span><span>TPS: <b id="topTps">0</b></span></div></header>
<main class="app">
<section class="left">
<div class="panel"><div class="ptitle">Simulation Controls</div><div class="pbody">
<div class="controls"><button id="pauseBtn" class="btn green">Pause</button><button id="resumeBtn" class="btn blue">Resume</button><button id="stopBtn" class="btn red">Stop</button></div>
<div class="row"><label>Agent delay</label><span class="value"><span id="delayValue">0</span> ms</span></div><input id="delay" type="range" min="0" max="300" value="120">
<label class="check"><input id="gridToggle" type="checkbox" checked> Show grid</label>
<label class="check"><input id="followToggle" type="checkbox"> Follow selected agent</label>
<label class="check"><input id="editWalls" type="checkbox"> Wall edit mode</label>
<div class="small">Wall edit: click an empty cell to add/remove a wall. Scroll over the board to zoom; drag to pan.</div>
</div></div>
<div class="panel"><div class="ptitle">Agent Types</div><div class="pbody legend"><span><i class="mark circle"></i>RoadRunner — burst</span><span><i class="mark tri"></i>Coyote — jump</span><span><i class="mark square"></i>Yosemite Sam — freeze</span><span><i class="flag">★</i>Flag / goal</span><span><i class="mark wall"></i>Wall / obstacle</span></div></div>
<div class="panel"><div class="ptitle">Selection</div><div class="pbody"><div class="row"><label>Agent ID</label><input id="agentSearch" type="number" min="0" value="0"></div><button id="selectAgentBtn" class="btn secondary" style="width:100%">Inspect agent</button><div class="small" style="margin-top:9px">Or click an agent directly in the simulation view.</div></div></div>
</section>
<section class="panel view"><div class="ptitle">Simulation View</div><div class="canvaswrap"><canvas id="sim"></canvas><div class="overlay"><div>Board: <span id="boardSize">-</span></div><div>Rendered: <span id="renderedCount">-</span></div><div>Zoom: <span id="zoomLabel">1.00×</span></div></div></div></section>
<section class="right">
<div class="panel"><div class="ptitle">Simulation Stats</div><div class="pbody stats"><span class="k">Agents</span><span class="v" id="statAgents">0</span><span class="k">Frozen</span><span class="v" id="statFrozen">0</span><span class="k">Board</span><span class="v" id="statBoard">-</span><span class="k">Steps</span><span class="v" id="statSteps">0</span><span class="k">Rolling TPS</span><span class="v" id="statTps">0</span><span class="k">Elapsed</span><span class="v" id="statElapsed">0.0 s</span></div></div>
<div class="panel"><div class="ptitle">Live Agent State</div><div class="pbody" id="agentPanel"><div class="small">Select an agent to inspect its current state.</div></div></div>
<div class="panel"><div class="ptitle">Spatial Occupancy</div><div class="pbody spatial" id="spatialPanel">Select an agent to inspect its direct cell index.</div></div>
</section>
<section class="bottom">
<div class="panel"><div class="ptitle">Throughput — Rolling</div><div class="chart"><canvas id="tpsChart"></canvas></div></div>
<div class="panel"><div class="ptitle">Frozen Agents — Live</div><div class="chart"><canvas id="frozenChart"></canvas></div></div>
</section>
<section class="panel notes"><div class="ptitle">Architecture Notes</div><div class="pbody note"><strong>Thread-per-agent:</strong> each active agent runs on its own OS thread.<br><br><strong>State safety:</strong> mutable board state is protected by the existing global <code>BoardLock</code> discipline.<br><br><strong>UI safety:</strong> the browser receives read-only snapshots. User commands are queued and applied by the simulation's main control loop instead of mutating board state from the HTTP thread.<br><br><strong>Benchmarks:</strong> <code>--benchmark</code> and <code>--sweep</code> remain headless and do not run this UI.</div><div class="footer">Local-only dashboard · bound to 127.0.0.1</div></section>
</main>
<script>
const $=id=>document.getElementById(id);let layout=null,state=null,selected=-1,lastSteps=0,lastPoll=performance.now(),rollingTps=0;let tpsHist=[],frozenHist=[];let zoom=1,panX=0,panY=0,drag=false,dragStart=null;let wallSet=new Set();
const roleColor=['#46a7ff','#ff5b4d','#ff9d3d'];
function fmtTime(sec){sec=Math.max(0,Math.floor(sec));const h=String(Math.floor(sec/3600)).padStart(2,'0'),m=String(Math.floor(sec%3600/60)).padStart(2,'0'),s=String(sec%60).padStart(2,'0');return `${h}:${m}:${s}`}
async function getJson(url,opt){const r=await fetch(url,opt);if(!r.ok)throw new Error(await r.text());return r.json()}
async function loadLayout(){layout=await getJson('/api/layout');wallSet=new Set(layout.walls.map(w=>w[0]+','+w[1]));$('boardSize').textContent=`${layout.rows} × ${layout.cols}`;$('statBoard').textContent=`${layout.rows}×${layout.cols}`;draw()}
async function command(cmd,args={}){const q=new URLSearchParams({cmd,...args});await getJson('/api/control?'+q.toString(),{method:'POST'});if(cmd==='addwall'||cmd==='removewall')await loadLayout()}
function updateStats(s){$('topSteps').textContent=s.totalSteps.toLocaleString();$('topTime').textContent=fmtTime(s.elapsedSec);$('statAgents').textContent=s.totalAgents.toLocaleString();$('statFrozen').textContent=s.frozenAgents.toLocaleString();$('statSteps').textContent=s.totalSteps.toLocaleString();$('statElapsed').textContent=s.elapsedSec.toFixed(1)+' s';$('renderedCount').textContent=`${s.displayedAgents.toLocaleString()} / ${s.totalAgents.toLocaleString()}`;$('delay').value=s.delayMs;$('delayValue').textContent=s.delayMs;const now=performance.now(),dt=(now-lastPoll)/1000,ds=s.totalSteps-lastSteps;if(lastSteps>0&&dt>0){const inst=ds/dt;rollingTps=rollingTps?rollingTps*.72+inst*.28:inst;tpsHist.push(rollingTps);frozenHist.push(s.frozenAgents);if(tpsHist.length>90)tpsHist.shift();if(frozenHist.length>90)frozenHist.shift()}lastSteps=s.totalSteps;lastPoll=now;$('topTps').textContent=Math.round(rollingTps).toLocaleString();$('statTps').textContent=Math.round(rollingTps).toLocaleString();const rs=$('runState');$('stopBtn').textContent=s.gameOver?'Close':'Stop';if(s.gameOver){rs.className='';rs.style.color='#ff9d3d';rs.innerHTML='<i class="dot"></i>Stopped'}else if(s.paused){rs.className='';rs.style.color='#ffd95a';rs.innerHTML='<i class="dot"></i>Paused'}else{rs.className='running';rs.style.color='';rs.innerHTML='<i class="dot"></i>Running'}drawCharts()}
function updateAgent(a){const p=$('agentPanel'),sp=$('spatialPanel');if(!a){p.innerHTML='<div class="small">Select an agent to inspect its current state.</div>';sp.textContent='Select an agent to inspect its direct cell index.';return}const role=['RoadRunner','Coyote','Yosemite Sam'][a.role]||'Unknown';const op=a.frozen?'FROZEN':'ACTIVE';const shoot=a.role===2?(a.canShoot?'READY':`${a.cooldownRemainingMs} ms`):'N/A';p.innerHTML=`<div class="stats"><span class="k">ID</span><span class="v">${a.id}</span><span class="k">Type</span><span class="v">${role}</span><span class="k">State</span><span class="v"><span class="badge ${a.frozen?'frozen':''}">${op}</span></span><span class="k">Position</span><span class="v">(${a.r}, ${a.c})</span><span class="k">Steps</span><span class="v">${a.steps.toLocaleString()}</span><span class="k">Freeze left</span><span class="v">${a.freezeRemainingMs} ms</span><span class="k">Shoot capability</span><span class="v">${shoot}</span></div><div class="agent-actions"><button class="btn blue" onclick="command('freeze',{id:${a.id},ms:1500})">Freeze 1.5s</button><button class="btn secondary" onclick="command('unfreeze',{id:${a.id}})">Unfreeze</button></div>`;const idx=a.r*state.cols+a.c;sp.innerHTML=`Cell: (${a.r}, ${a.c})<br>Index: ${idx}<br>occupant[index]: ${a.id}<br><span class="small">Direct O(1) occupancy mapping</span>`}
async function poll(){try{state=await getJson('/api/state?selected='+selected);updateStats(state);updateAgent(state.selected);if($('followToggle').checked&&state.selected&&layout){const c=cellGeometry();panX=c.viewW/2-(state.selected.c+.5)*c.cw*zoom;panY=c.viewH/2-(state.selected.r+.5)*c.ch*zoom}draw()}catch(e){console.error(e)}setTimeout(poll,150)}
function resizeCanvas(canvas){const dpr=window.devicePixelRatio||1,rect=canvas.getBoundingClientRect();const w=Math.max(1,Math.floor(rect.width*dpr)),h=Math.max(1,Math.floor(rect.height*dpr));if(canvas.width!==w||canvas.height!==h){canvas.width=w;canvas.height=h}return{w,h,dpr}}
function cellGeometry(){const c=$('sim'),{w,h}=resizeCanvas(c),cw=w/layout.cols,ch=h/layout.rows;return{viewW:w,viewH:h,cw,ch}}
function draw(){if(!layout||!state)return;const c=$('sim'),ctx=c.getContext('2d'),g=cellGeometry();ctx.clearRect(0,0,g.viewW,g.viewH);ctx.save();ctx.translate(panX,panY);ctx.scale(zoom,zoom);ctx.fillStyle='#060c13';ctx.fillRect(0,0,g.viewW/zoom,g.viewH/zoom);for(const [r,col] of layout.walls){ctx.fillStyle='#596775';ctx.fillRect(col*g.cw,r*g.ch,Math.ceil(g.cw),Math.ceil(g.ch))}if($('gridToggle').checked&&(g.cw*zoom>5&&g.ch*zoom>5)){ctx.strokeStyle='rgba(83,111,137,.20)';ctx.lineWidth=1/zoom;ctx.beginPath();for(let x=0;x<=layout.cols;x++){ctx.moveTo(x*g.cw,0);ctx.lineTo(x*g.cw,layout.rows*g.ch)}for(let y=0;y<=layout.rows;y++){ctx.moveTo(0,y*g.ch);ctx.lineTo(layout.cols*g.cw,y*g.ch)}ctx.stroke()}const fr=layout.flag.r,fc=layout.flag.c;ctx.fillStyle='#ffd95a';ctx.font=`${Math.max(10,Math.min(g.cw,g.ch)*1.1)}px sans-serif`;ctx.textAlign='center';ctx.textBaseline='middle';ctx.fillText('★',(fc+.5)*g.cw,(fr+.5)*g.ch);for(const a of state.agents){const x=(a.c+.5)*g.cw,y=(a.r+.5)*g.ch,rad=Math.max(2,Math.min(g.cw,g.ch)*.34);ctx.fillStyle=roleColor[a.role]||'#fff';ctx.strokeStyle=a.id===selected?'#fff':'rgba(0,0,0,.3)';ctx.lineWidth=(a.id===selected?2.5:1)/zoom;ctx.beginPath();if(a.role===0){ctx.arc(x,y,rad,0,Math.PI*2)}else if(a.role===1){ctx.moveTo(x,y-rad);ctx.lineTo(x+rad,y+rad);ctx.lineTo(x-rad,y+rad);ctx.closePath()}else{ctx.rect(x-rad,y-rad,rad*2,rad*2)}ctx.fill();ctx.stroke();if(a.frozen&&rad>3){ctx.strokeStyle='#9bd7ff';ctx.lineWidth=2/zoom;ctx.beginPath();ctx.arc(x,y,rad*1.45,0,Math.PI*2);ctx.stroke()}}ctx.restore();$('zoomLabel').textContent=zoom.toFixed(2)+'×'}
function drawLineChart(canvas,data,color){const ctx=canvas.getContext('2d'),{w,h}=resizeCanvas(canvas);ctx.clearRect(0,0,w,h);ctx.strokeStyle='#2b4156';ctx.lineWidth=1;for(let i=1;i<4;i++){const y=h*i/4;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke()}if(data.length<2)return;let max=Math.max(...data,1),min=Math.min(...data,0);if(max===min)max=min+1;ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();data.forEach((v,i)=>{const x=i/(Math.max(1,data.length-1))*w,y=h-((v-min)/(max-min))*(h*.82)-h*.08;i?ctx.lineTo(x,y):ctx.moveTo(x,y)});ctx.stroke();ctx.fillStyle='#8fa3b8';ctx.font=`${12*(window.devicePixelRatio||1)}px sans-serif`;ctx.fillText(Math.round(max).toLocaleString(),6,15*(window.devicePixelRatio||1));ctx.fillText(Math.round(min).toLocaleString(),6,h-5)}
function drawCharts(){drawLineChart($('tpsChart'),tpsHist,'#35d07f');drawLineChart($('frozenChart'),frozenHist,'#b47cff')}
function canvasToCell(evt){if(!layout)return null;const c=$('sim'),rect=c.getBoundingClientRect(),dpr=window.devicePixelRatio||1,g=cellGeometry();let x=(evt.clientX-rect.left)*dpr,y=(evt.clientY-rect.top)*dpr;x=(x-panX)/zoom;y=(y-panY)/zoom;const col=Math.floor(x/g.cw),r=Math.floor(y/g.ch);if(r<0||r>=layout.rows||col<0||col>=layout.cols)return null;return{r,c:col}}
$('pauseBtn').onclick=()=>command('pause');$('resumeBtn').onclick=()=>command('resume');$('stopBtn').onclick=()=>command('stop');$('delay').oninput=e=>{$('delayValue').textContent=e.target.value};$('delay').onchange=e=>command('speed',{ms:e.target.value});$('selectAgentBtn').onclick=()=>{selected=Math.max(-1,parseInt($('agentSearch').value||'-1'));};$('gridToggle').onchange=draw;
const sim=$('sim');sim.addEventListener('wheel',e=>{e.preventDefault();const old=zoom;zoom=Math.max(.5,Math.min(12,zoom*(e.deltaY<0?1.15:.87)));const rect=sim.getBoundingClientRect(),dpr=window.devicePixelRatio||1,mx=(e.clientX-rect.left)*dpr,my=(e.clientY-rect.top)*dpr;panX=mx-(mx-panX)*(zoom/old);panY=my-(my-panY)*(zoom/old);draw()},{passive:false});sim.addEventListener('mousedown',e=>{if(e.button===1||e.shiftKey){drag=true;dragStart={x:e.clientX,y:e.clientY,px:panX,py:panY}}});window.addEventListener('mouseup',()=>drag=false);window.addEventListener('mousemove',e=>{if(!drag)return;const dpr=window.devicePixelRatio||1;panX=dragStart.px+(e.clientX-dragStart.x)*dpr;panY=dragStart.py+(e.clientY-dragStart.y)*dpr;draw()});sim.addEventListener('click',async e=>{if(drag)return;const cell=canvasToCell(e);if(!cell)return;if($('editWalls').checked){const key=cell.r+','+cell.c;if(wallSet.has(key))await command('removewall',{r:cell.r,c:cell.c});else await command('addwall',{r:cell.r,c:cell.c});return}if(!state)return;let found=null;for(const a of state.agents)if(a.r===cell.r&&a.c===cell.c){found=a;break}if(found){selected=found.id;$('agentSearch').value=found.id}});window.addEventListener('resize',()=>{draw();drawCharts()});
(async()=>{await loadLayout();poll()})().catch(console.error);
</script>
</body></html>
)HTML";

#ifdef __linux__
static bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}
#endif

} // namespace

WebUiServer::WebUiServer(int port,
                         LayoutProvider layoutProvider,
                         StateProvider stateProvider,
                         CommandSink commandSink)
    : port_(port),
      layoutProvider_(std::move(layoutProvider)),
      stateProvider_(std::move(stateProvider)),
      commandSink_(std::move(commandSink)) {}

WebUiServer::~WebUiServer() {
    stop();
}

bool WebUiServer::start() {
#ifdef __linux__
    if (serverThread_.joinable()) return true;
    stopping_.store(false);

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "UI: socket() failed: " << std::strerror(errno) << "\n";
        return false;
    }

    int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int flags = ::fcntl(listenFd_, F_GETFL, 0);
    ::fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "UI: bind(127.0.0.1:" << port_ << ") failed: " << std::strerror(errno) << "\n";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 16) < 0) {
        std::cerr << "UI: listen() failed: " << std::strerror(errno) << "\n";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    serverThread_ = std::thread(&WebUiServer::run, this);
    return true;
#else
    std::cerr << "UI: web dashboard currently supports Linux only.\n";
    return false;
#endif
}

void WebUiServer::stop() {
    stopping_.store(true);
#ifdef __linux__
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
#endif
    if (serverThread_.joinable()) serverThread_.join();
}

void WebUiServer::run() {
#ifdef __linux__
    while (!stopping_.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int client = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }
            if (!stopping_.load()) std::cerr << "UI: accept() failed: " << std::strerror(errno) << "\n";
            break;
        }

        timeval tv{};
        tv.tv_sec = 1;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string req;
        char buf[4096];
        for (;;) {
            ssize_t n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            if (req.find("\r\n\r\n") != std::string::npos || req.size() > 16384) break;
        }

        std::string response;
        std::istringstream requestStream(req);
        std::string method, target, version;
        requestStream >> method >> target >> version;

        std::string path = target;
        std::string query;
        size_t qpos = target.find('?');
        if (qpos != std::string::npos) {
            path = target.substr(0, qpos);
            query = target.substr(qpos + 1);
        }
        auto params = parseQuery(query);

        try {
            if (method == "GET" && path == "/") {
                response = httpResponse(kDashboardHtml, "text/html; charset=utf-8");
            } else if (method == "GET" && path == "/api/layout") {
                response = httpResponse(layoutJson(layoutProvider_()));
            } else if (method == "GET" && path == "/api/state") {
                int selected = intParam(params, "selected", -1);
                response = httpResponse(stateJson(stateProvider_(selected)));
            } else if (method == "POST" && path == "/api/control") {
                auto it = params.find("cmd");
                if (it == params.end()) {
                    response = httpResponse("{\"ok\":false,\"error\":\"missing cmd\"}",
                                            "application/json; charset=utf-8", "400 Bad Request");
                } else {
                    UiCommand c;
                    bool valid = true;
                    if (it->second == "pause") c.type = UiCommandType::Pause;
                    else if (it->second == "resume") c.type = UiCommandType::Resume;
                    else if (it->second == "stop") c.type = UiCommandType::Stop;
                    else if (it->second == "speed") {
                        c.type = UiCommandType::SetDelayMs;
                        c.value = intParam(params, "ms", 0);
                    } else if (it->second == "freeze") {
                        c.type = UiCommandType::FreezeAgent;
                        c.agentId = intParam(params, "id", -1);
                        c.value = intParam(params, "ms", 1500);
                    } else if (it->second == "unfreeze") {
                        c.type = UiCommandType::UnfreezeAgent;
                        c.agentId = intParam(params, "id", -1);
                    } else if (it->second == "addwall") {
                        c.type = UiCommandType::AddWall;
                        c.r = intParam(params, "r", -1);
                        c.c = intParam(params, "c", -1);
                    } else if (it->second == "removewall") {
                        c.type = UiCommandType::RemoveWall;
                        c.r = intParam(params, "r", -1);
                        c.c = intParam(params, "c", -1);
                    } else valid = false;

                    if (!valid) {
                        response = httpResponse("{\"ok\":false,\"error\":\"unknown command\"}",
                                                "application/json; charset=utf-8", "400 Bad Request");
                    } else {
                        commandSink_(c);
                        response = httpResponse("{\"ok\":true}");
                    }
                }
            } else {
                response = httpResponse("{\"error\":\"not found\"}",
                                        "application/json; charset=utf-8", "404 Not Found");
            }
        } catch (const std::exception& e) {
            response = httpResponse(std::string("{\"ok\":false,\"error\":\"") +
                                    jsonEscape(e.what()) + "\"}",
                                    "application/json; charset=utf-8", "500 Internal Server Error");
        }

        sendAll(client, response);
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }
#endif
}

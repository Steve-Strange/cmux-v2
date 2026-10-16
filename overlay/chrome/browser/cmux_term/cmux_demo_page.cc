// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_demo_page.h"

#include <string>

#include "base/base64.h"
#include "base/strings/strcat.h"

namespace cmux {

// Rich self-contained dogfood page: many input types, contenteditable,
// drag-and-drop reorder + drop zone, buttons with live state, and a long
// scroll region. Returned as a base64 data: URL so startup does no blocking
// file I/O on the UI thread (fatal under dcheck_always_on).
GURL CmuxDemoURL() {
  static const char kHtml[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>cmux input test</title><style>
 :root{color-scheme:light dark}*{box-sizing:border-box}
 body{font:14px -apple-system,system-ui,sans-serif;margin:0;background:#0e0f12;color:#e6e7ea}
 header{position:sticky;top:0;background:#16181d;padding:12px 18px;border-bottom:1px solid #2a2d34;z-index:5}
 h1{margin:0;font-size:15px}.sub{color:#8b90a0;font-size:12px}
 section{padding:16px 18px;border-bottom:1px solid #20232a}
 h2{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:#9aa0b0;margin:0 0 10px}
 label{display:block;margin:8px 0 4px;color:#b9bdc9}
 input,textarea,select{width:100%;max-width:420px;padding:8px 10px;border-radius:8px;border:1px solid #343843;background:#1b1e25;color:#e6e7ea;font:inherit}
 input[type=checkbox],input[type=radio],input[type=range],input[type=color]{width:auto}
 textarea{min-height:80px}
 .row{display:flex;gap:14px;flex-wrap:wrap;align-items:center}
 button{padding:8px 14px;border-radius:8px;border:1px solid #3a3f4b;background:#2563eb;color:#fff;font:inherit;cursor:pointer}
 .pill{padding:2px 10px;border-radius:999px;background:#1f2430;color:#9aa0b0}
 ul.drag{list-style:none;padding:0;margin:0;max-width:420px}
 ul.drag li{padding:10px 12px;margin:6px 0;border-radius:8px;background:#1b1e25;border:1px solid #2a2d34;cursor:grab}
 ul.drag li.dragging{opacity:.4}
 .zone{margin-top:12px;padding:24px;border:2px dashed #3a3f4b;border-radius:12px;text-align:center;color:#8b90a0}
 .zone.over{border-color:#2563eb;color:#cbd2e6;background:#141a2b}
 [contenteditable]{min-height:54px;padding:10px;border-radius:8px;border:1px solid #343843;background:#1b1e25;max-width:420px}
 .filler p{color:#6b7180}
</style></head><body>
<header><h1>cmux input test</h1><div class="sub">type, select, drag &amp; drop, scroll — verify input works</div></header>
<section><h2>Text</h2>
 <label>Text</label><input type="text" placeholder="type here, tab between fields" autofocus>
 <label>Email</label><input type="email" placeholder="you@example.com">
 <label>Password</label><input type="password" placeholder="secret">
 <label>Search</label><input type="search" placeholder="search">
 <label>Number</label><input type="number" value="42">
 <label>Textarea</label><textarea placeholder="multi-line"></textarea>
 <label>Contenteditable</label><div contenteditable="true">Edit me.</div>
</section>
<section><h2>Controls</h2>
 <label>Select</label><select><option>Alpha</option><option>Bravo</option><option>Charlie</option></select>
 <div class="row" style="margin-top:10px">
  <label><input type="checkbox" checked> Checkbox</label>
  <label><input type="radio" name="r" checked> Radio 1</label>
  <label><input type="radio" name="r"> Radio 2</label>
  <label>Range <input type="range" min="0" max="100" value="30" id="rng"></label><span class="pill" id="rv">30</span>
  <label>Color <input type="color" value="#2563eb"></label>
  <label>File <input type="file"></label>
  <label>Date <input type="date"></label>
 </div>
</section>
<section><h2>Buttons</h2>
 <div class="row"><button id="inc">Increment</button><button id="dec" style="background:#2b2f38">Decrement</button>
  <span class="pill">count: <span id="c">0</span></span></div>
</section>
<section><h2>Drag &amp; drop</h2>
 <ul class="drag" id="list"><li draggable="true">Drag 1</li><li draggable="true">Drag 2</li><li draggable="true">Drag 3</li><li draggable="true">Drag 4</li></ul>
 <div class="zone" id="zone">Drop here</div>
</section>
<section class="filler"><h2>Scroll</h2><div id="f"></div></section>
<script>
 var n=0,c=document.getElementById('c');
 document.getElementById('inc').onclick=function(){c.textContent=++n};
 document.getElementById('dec').onclick=function(){c.textContent=--n};
 var rng=document.getElementById('rng');rng.oninput=function(){document.getElementById('rv').textContent=rng.value};
 var list=document.getElementById('list'),drag=null;
 list.addEventListener('dragstart',function(e){drag=e.target;e.target.classList.add('dragging')});
 list.addEventListener('dragend',function(e){e.target.classList.remove('dragging');drag=null});
 list.addEventListener('dragover',function(e){e.preventDefault();var a=null,it=[].slice.call(list.querySelectorAll('li:not(.dragging)'));
  for(var i=0;i<it.length;i++){var r=it[i].getBoundingClientRect();if(e.clientY<r.top+r.height/2){a=it[i];break}}
  if(drag){a?list.insertBefore(drag,a):list.appendChild(drag)}});
 var z=document.getElementById('zone');
 z.addEventListener('dragover',function(e){e.preventDefault();z.classList.add('over')});
 z.addEventListener('dragleave',function(){z.classList.remove('over')});
 z.addEventListener('drop',function(e){e.preventDefault();z.classList.remove('over');z.textContent='Dropped: '+(drag?drag.textContent:'item')});
 var s='';for(var i=1;i<=50;i++)s+='<p>Scroll line '+i+' — wheel + momentum + scrollbars.</p>';document.getElementById('f').innerHTML=s;
</script></body></html>)HTML";
  const std::string b64 =
      base::Base64Encode(std::string_view(kHtml, sizeof(kHtml) - 1));
  return GURL(base::StrCat({"data:text/html;charset=utf-8;base64,", b64}));
}

}  // namespace cmux

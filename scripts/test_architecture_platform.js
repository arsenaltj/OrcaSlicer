// Offline smoke test of the actual generated explorer script. No browser,
// network, external service or product runtime is started.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const config = JSON.parse(fs.readFileSync(path.join(root, 'Docs/architecture/review-map.json'), 'utf8'));
const neutral = () => ({kind:'unchanged', changed:[], potential:[]});
const report = {
  mode:'工作区（含未提交修改）', base:'a'.repeat(40), head:'b'.repeat(40), changes:[], potential_files:[],
  modules:config.modules.map(m=>({...m, changed:0, potential:0})), potential_by_module:{},
  business:{
    evidence:config.evidence.map(e=>({...e, found:true, line:1, sha256:'fixture-only'})),
    journeys:config.journeys.map(j=>({...j, states:[], phases:j.phases.map(p=>({...p,impact:neutral()}))})),
    flows:config.business_flows.map(f=>({...f,impact:neutral()})), overview:config.product_overview,
  },
  catalog:{...config.landscape,
    capabilities:config.landscape.capabilities.map(c=>({...c, files:['src/'+c.id+'.cpp'], test_files:[], lines:2, indexed_files:0, impact:neutral()})),
    dependencies:[], files:[{path:'src/desktop.cpp', lines:2, indexed:false, sha256:'snapshot-file-identity'}],
    coverage:{inventory_files:1,indexed_files:0,mapped_files:1,curated_files:1,directory_default_files:0,unindexed_files:1},
    unassigned_files:[], limits:['fixture only'], inventory_identity:'inventory-identity',
  },
};

class Element {
  constructor(tag='div') {
    this.tag=tag; this.children=[]; this._text=''; this.className=''; this.listeners={}; this.style={};
    this.value=''; this.open=false;
    this.classList={add:(...v)=>{this.className+=' '+v.join(' ');},remove:(...v)=>{this.className=this.className.split(' ').filter(c=>!v.includes(c)).join(' ');}};
  }
  set textContent(v){this._text=String(v); this.children=[];}
  get textContent(){return this._text+this.children.map(c=>c.textContent).join(' ');}
  get childElementCount(){return this.children.length;}
  append(...children){this.children.push(...children);}
  prepend(...children){this.children.unshift(...children);}
  replaceChildren(...children){this._text=''; this.children=[...children];}
  insertBefore(child,before){const index=this.children.indexOf(before); assert.ok(index>=0); this.children.splice(index,0,child);}
  addEventListener(name,fn){(this.listeners[name]??=[]).push(fn);}
  setAttribute(name,value){this[name]=value;}
  focus(){}
  getBoundingClientRect(){return {left:0,top:0,width:1000,height:800};}
  querySelector(selector){return this.children.find(c=>c.className.split(' ').includes(selector.slice(1))) || this.children.map(c=>c.querySelector(selector)).find(Boolean);}
  showModal(){this.open=true;}
  close(){this.open=false;}
}
const ids=Object.fromEntries(['data','stage','crumb','back','home','archify','inspector','announcement','diagram-dialog','diagram-frame','close-diagram','version','comparison'].map(id=>['#'+id,new Element()]));
ids['#data'].textContent=JSON.stringify(report);
const document={querySelector:q=>{assert.ok(ids[q],'unexpected selector '+q); return ids[q];},createElement:tag=>new Element(tag),addEventListener:()=>{}};
const context=vm.createContext({document,window:{scrollTo:()=>{}},matchMedia:()=>({matches:true}),console});
let html=fs.readFileSync(path.join(__dirname,'architecture_explorer.html'),'utf8');
html=html.replace('__PLATFORM_SCRIPT__',fs.readFileSync(path.join(__dirname,'architecture_platform.js'),'utf8'));
const scripts=[...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)].map(m=>m[1]).filter(s=>!s.includes('__REPORT_DATA__'));
assert.equal(scripts.length,1);
vm.runInContext(scripts[0],context);
assert.match(ids['#stage'].textContent,/先看平台职责/);
assert.equal(vm.runInContext('document.querySelector("#stage").children[2].children.length',context),5);
let views=0;
for(const capability of config.landscape.capabilities){
  vm.runInContext(`open({kind:'capability',id:${JSON.stringify(capability.id)},label:'test'})`,context);
  assert.match(ids['#stage'].textContent,/状态由谁维护/);
  assert.ok(ids['#stage'].textContent.includes(capability.label));
  views++;
}
for(const scenario of config.landscape.scenarios){
  vm.runInContext(`open({kind:'scenario',id:${JSON.stringify(scenario.id)},label:'test'})`,context);
  assert.ok(ids['#stage'].textContent.includes(scenario.verify)); views++;
}
vm.runInContext("open({kind:'coverage',label:'覆盖'})",context);
assert.match(ids['#stage'].textContent,/目录默认归属/); views++;
vm.runInContext("open({kind:'business',label:'业务'})",context);
assert.match(ids['#stage'].textContent,/从 AI 能力到打印结果/); views++;
vm.runInContext("inspectFile('src/desktop.cpp')",context);
assert.match(ids['#inspector'].textContent,/snapshot-file-identity/);
vm.runInContext("backTo(0)",context);
assert.match(ids['#stage'].textContent,/先看平台职责/);
console.log(JSON.stringify({status:'PASS',views,capabilities:config.landscape.capabilities.length,checks:['root','capabilities','scenarios','coverage','legacy business','source identity','back navigation']}));

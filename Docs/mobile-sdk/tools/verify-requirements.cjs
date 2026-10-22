'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),cp=require('node:child_process');
const {pathToFileURL}=require('node:url');
const mobileFiles=['lib','packages/printer_client','packages/orca_sdk_bridge','packages/platform_services','android','ios','integration_test','test','tool','pubspec.yaml'];
const platformFiles=['src/modules','contracts/http/openapi.yaml','contracts/events','db/migrations','db/seeds','deploy','scripts','test','package.json'];
function all(source,markers){return markers.every(marker=>source.includes(marker));}
function checkDocs({md,html,topology,mobilePaths,platformPaths}){
 const ids=new Set([...html.matchAll(/\bid=["']([^"']+)["']/g)].map(m=>m[1]));const anchors=[...html.matchAll(/href=["']#([^"']+)["']/g)].map(m=>decodeURIComponent(m[1]));
 const count=expression=>(html.match(expression)||[]).length;
 return {
  utf8_no_replacement:![md,html,topology].some(s=>s.includes('\uFFFD')),
  html_tables_balanced:count(/<table\b/gi)>0&&count(/<table\b/gi)===count(/<\/table>/gi)&&count(/<pre\b/gi)===count(/<\/pre>/gi)&&count(/<h2\b/gi)===count(/<\/h2>/gi),
  toc_targets:anchors.length>0&&anchors.every(id=>ids.has(id)),
  mobile_actual_paths:mobileFiles.every(p=>mobilePaths[p]===true),
  platform_actual_paths:platformFiles.every(p=>platformPaths[p]===true),
  report_uses_actual_mobile_paths:all(md,['lib/features','packages/printer_client','packages/orca_sdk_bridge','packages/platform_services','android/','ios/','integration_test/','tool/']),
  report_uses_actual_platform_paths:all(md,['src/modules','contracts/','openapi.yaml','db/','migrations/','deploy/','test/','NestJS 模块化单体基线']),
  report_labels_future_backend_split:all(md,['当前没有 '+String.fromCharCode(96)+'apps/api/'+String.fromCharCode(96),'后续扩展方向','不能作为当前实现状态描述']),
  monorepo_layout:['orca-frontend','apps/web','apps/admin','编译隔离'].every(p=>md.includes(p)&&topology.includes(p)),
  no_separate_frontend_repos:!['orca-web','orca-admin'].some(p=>(md+topology).includes(p)),
  compile_isolation:all(md+topology,['独立应用','独立编译目标','共享包不能反向依赖','独立构建配置']),
  independent_admin_boundary:all(md,['独立管理员登录','OpenAPI / Events']),
  vendor_facts_recorded:all(md,['WebSocket + HTTP','MQTT + TLS','Kinesis Video Streams','6 位随机口令','主动 TLS 出站']),
  requirement_ids:all(md,['P1-VENDOR-01','P1-CONN-02','P1-FILE-01','P1-QA-02','P1-WEB-01','P1-ADMIN-01','P2-CONN-03','P2-FILE-04','P2-MEDIA-05','P2-OBS-06','P2-VENDOR-07','P2-WEB-08','P3-CONN-04','P3-EDGE-05']),
  phase_counts_current:all(md,['一期 13+6','二期 11+6','三期 10+2']),
  no_old_phase_counts:!['一期 13+4','二期 11+5'].some(p=>md.includes(p)),
  no_real_moonraker_claim:md.includes('不能把 Moonraker 作为 WonderMaker 真实协议'),
  version_1_7:md.includes('文档版本：1.7')&&html.includes('v1.7')
 };
}
function git(root,args){const r=cp.spawnSync('git',['--no-optional-locks','-C',root,...args],{encoding:'utf8',windowsHide:true});return r.status===0?r.stdout.trim():null;}
function workspaceFor(worktree){let current=path.resolve(worktree);while(true){if(fs.existsSync(path.join(current,'control/workspace.yaml'))&&fs.existsSync(path.join(current,'AGENTS.md')))return current;const parent=path.dirname(current);if(parent===current)throw new Error('Specify --workspace for this checkout.');current=parent;}}
async function browserCheck(file,output,modulePath){
 const {chromium}=require(modulePath||process.env.CODEX_PLAYWRIGHT_MODULE||'playwright');const browser=await chromium.launch({channel:process.env.CODEX_BROWSER_CHANNEL||'msedge',headless:true});const scenarios=[];
 try{for(const width of [1440,390]){const page=await browser.newPage({viewport:{width,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));await page.goto(pathToFileURL(file).href,{waitUntil:'load'});
  const observed=await page.evaluate(()=>({viewport:innerWidth,documentWidth:document.documentElement.scrollWidth,missingAnchors:[...document.querySelectorAll('a[href^="#"]')].map(a=>a.getAttribute('href').slice(1)).filter(id=>id&&!document.getElementById(decodeURIComponent(id))),missingImages:[...document.images].filter(i=>!i.complete||i.naturalWidth===0).map(i=>i.getAttribute('src'))}));
  await page.screenshot({path:path.join(output,'requirements-'+width+'.png')});scenarios.push({width,...observed,errors,ok:observed.documentWidth<=observed.viewport&&observed.missingAnchors.length===0&&observed.missingImages.length===0&&errors.length===0});await page.close();}
 }finally{await browser.close();}return{executed:true,checkedAt:new Date().toISOString(),scenarios,ok:scenarios.every(s=>s.ok)};
}
async function verify(options){
 const worktree=path.resolve(options.worktree||path.join(__dirname,'../../..')),workspace=path.resolve(options.workspace||workspaceFor(worktree)),output=path.resolve(options.output);fs.mkdirSync(output,{recursive:true});if(fs.existsSync(path.join(output,'validation.json')))throw new Error('Choose a new output directory; previous validation evidence is retained.');
 const docs=path.join(worktree,'docs/mobile-sdk'),mdFile=path.join(docs,'REQUIREMENTS-ANALYSIS.md'),htmlFile=path.join(docs,'REQUIREMENTS-ANALYSIS.html'),topFile=path.join(docs,'PROJECT-TOPOLOGY.md');
 const input={md:fs.readFileSync(mdFile,'utf8'),html:fs.readFileSync(htmlFile,'utf8'),topology:fs.readFileSync(topFile,'utf8'),mobilePaths:Object.fromEntries(mobileFiles.map(p=>[p,fs.existsSync(path.join(workspace,'repos/orca-mobile',p))])),platformPaths:Object.fromEntries(platformFiles.map(p=>[p,fs.existsSync(path.join(workspace,'repos/orca-platform',p))]))};
 const checks=checkDocs(input),taskId=path.basename(worktree),artifact=path.join(workspace,'artifacts',taskId),revision=path.join(artifact,'evidence/requirements-revision-20261019');
 const files=[mdFile,htmlFile,topFile,path.join(artifact,'reports/requirements-analysis-v1.7.md'),path.join(artifact,'reports/requirements-analysis-v1.7.html'),path.join(revision,'vendor-reply-summary.md'),path.join(revision,'implementation-status.md'),path.join(revision,'local-repo-structure.md')].map(file=>{const bytes=fs.readFileSync(file);return{path:file,bytes:bytes.length,sha256:crypto.createHash('sha256').update(bytes).digest('hex')};});
 const browser=options.browser?await browserCheck(htmlFile,output,options.playwrightModule):{executed:false,ok:null};const diff=git(worktree,['diff','--binary'])||'';
 const result={checkedAt:new Date().toISOString(),scope:'文档内容、实际目录、静态结构与浏览器渲染；不代表原业务代码或真实设备集成重新验收',taskId,reportVersion:'1.7',checks,failedChecks:Object.entries(checks).filter(([,ok])=>!ok).map(([name])=>name),browser,files,sourceGit:{head:git(worktree,['rev-parse','HEAD']),status:git(worktree,['status','--porcelain','--untracked-files=no']),diffSha256:crypto.createHash('sha256').update(diff).digest('hex')},ok:Object.values(checks).every(Boolean)&&(!options.browser||browser.ok===true)};
 fs.writeFileSync(path.join(output,'validation.json'),JSON.stringify(result,null,2));return result;
}
function parse(args){const out={};for(let i=0;i<args.length;i++){if(args[i]==='--browser')out.browser=true;else if(['--worktree','--workspace','--output','--playwright-module'].includes(args[i])){const key=args[i].slice(2).replace(/-([a-z])/g,(_,s)=>s.toUpperCase());out[key]=args[++i];}else throw new Error('Unknown option: '+args[i]);}if(!out.output)throw new Error('Specify --output with a new evidence directory.');return out;}
if(require.main===module)verify(parse(process.argv.slice(2))).then(result=>{console.log(JSON.stringify({ok:result.ok,checks:result.checks,failedChecks:result.failedChecks,browser:result.browser,output:process.argv[process.argv.indexOf('--output')+1]},null,2));if(!result.ok)process.exitCode=1;}).catch(error=>{console.error(error);process.exitCode=1;});
module.exports={checkDocs,verify,mobileFiles,platformFiles};

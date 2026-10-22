'use strict';
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');
const crypto = require('node:crypto');
const root = path.resolve(__dirname, '..');
const build = path.join(__dirname, 'build-android-arm64');
const evidence = process.env.ORCA_SLICING_EVIDENCE || path.resolve(root, '..', '..', '..', 'artifacts', 'task-20261020172216196-253d95', 'evidence', 'native-v2');
fs.mkdirSync(evidence, { recursive: true });
const cmake = process.env.CMAKE || 'cmake';
const ninja = process.env.NINJA || 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe';
const ndk = process.env.ANDROID_NDK || 'C:/Users/1/AppData/Local/Android/Sdk/ndk/28.2.13676358';
const log = path.join(evidence, 'android-arm64-build.log');
function git(args) { return cp.execFileSync('git', ['-C',root,...args], {encoding:'utf8',windowsHide:true}).trim(); }
function sha(bytes) { return crypto.createHash('sha256').update(bytes).digest('hex'); }
function run(args) {
 const fd = fs.openSync(log,'a');
 const r = cp.spawnSync(cmake,args,{cwd:root,stdio:['ignore',fd,fd],windowsHide:true});
 fs.closeSync(fd);
 if(r.status!==0) throw new Error('CMake failed ('+r.status+'); see '+log);
}
if(!process.argv.includes('--verify-only')) {
 run(['-S',path.join(root,'mobile-slicing'),'-B',build,'-G','Ninja','-DCMAKE_MAKE_PROGRAM='+ninja,'-DCMAKE_TOOLCHAIN_FILE='+path.join(ndk,'build/cmake/android.toolchain.cmake'),'-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-24','-DANDROID_STL=c++_static','-DCMAKE_BUILD_TYPE=Release']);
 run(['--build',build,'--target','orca_mobile_slicing_sdk','orca_mobile_stl_gate','orca_mobile_slicing_json_gate','-j2']);
}
const library=path.join(build,'liborca_mobile_slicing_sdk_c.so');
const elf=fs.readFileSync(library);
if(elf.subarray(0,4).toString('hex')!=='7f454c46'||elf[4]!==2||elf[5]!==1) throw new Error('Expected little-endian ELF64');
const offset=Number(elf.readBigUInt64LE(32)), size=elf.readUInt16LE(54), count=elf.readUInt16LE(56);
const loads=[];
for(let i=0;i<count;i++){const o=offset+i*size;if(elf.readUInt32LE(o)!==1)continue;const fileOffset=elf.readBigUInt64LE(o+8),virtualAddress=elf.readBigUInt64LE(o+16),alignment=elf.readBigUInt64LE(o+48);if(alignment<16384n||fileOffset%16384n!==virtualAddress%16384n)throw new Error('ELF LOAD fails 16KiB alignment');loads.push({fileOffset:String(fileOffset),virtualAddress:String(virtualAddress),alignment:String(alignment)});}
if(!loads.length)throw new Error('No ELF LOAD entries');
const readelf=path.join(ndk,'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe');
const symbols=cp.execFileSync(readelf,['--dyn-syms',library],{encoding:'utf8',maxBuffer:8*1024*1024,windowsHide:true});
for(const name of ['orca_mobile_slicing_create','orca_mobile_slicing_execute','orca_mobile_slicing_cancel','orca_mobile_slicing_destroy'])if(!symbols.includes(name))throw new Error('Missing exported symbol '+name);
const sourceCommit=git(['rev-parse','HEAD']);
if(!elf.includes(Buffer.from(sourceCommit)))throw new Error('ELF does not contain current configured source commit');
const packageDir=path.join(build,'package-arm64-v8a');
fs.mkdirSync(packageDir,{recursive:true});
const packagedLibrary=path.join(packageDir,'liborca_mobile_slicing_sdk_c.so');
const strip=path.join(ndk,'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe');
cp.execFileSync(strip,['--strip-unneeded',library,'-o',packagedLibrary],{windowsHide:true});
const packaged=fs.readFileSync(packagedLibrary);
if(!packaged.includes(Buffer.from(sourceCommit)))throw new Error('Stripped library lost source identity');
const systemLibraries=new Set(['liblog.so','libm.so','libdl.so','libc.so','libandroid.so','libz.so','libEGL.so','libGLESv2.so']);
const sharedFiles=new Map();
function discover(directory) {
 for(const entry of fs.readdirSync(directory,{withFileTypes:true})){
  if(entry.name==='CMakeFiles'||entry.name==='package-arm64-v8a'||entry.name.endsWith('-src')||entry.name.endsWith('-subbuild'))continue;
  const p=path.join(directory,entry.name);
  if(entry.isDirectory())discover(p); else if(entry.isFile()&&entry.name.endsWith('.so')&&!entry.name.includes('.stripped.'))sharedFiles.set(entry.name,p);
 }
}
discover(build);
const queue=[library],seen=new Set([path.basename(library)]),dependencies=[];
while(queue.length){
 const from=queue.shift();
 const dynamic=cp.execFileSync(readelf,['-d',from],{encoding:'utf8',windowsHide:true});
 for(const match of dynamic.matchAll(/Shared library: \[([^\]]+)\]/g)){
  const name=match[1];if(systemLibraries.has(name)||seen.has(name))continue;
  const dependency=sharedFiles.get(name);if(!dependency)throw new Error('Missing runtime dependency '+name);
  seen.add(name);queue.push(dependency);
  const destination=path.join(packageDir,name);
  cp.execFileSync(strip,['--strip-unneeded',dependency,'-o',destination],{windowsHide:true});
  const data=fs.readFileSync(destination);const po=Number(data.readBigUInt64LE(32)),ps=data.readUInt16LE(54),pn=data.readUInt16LE(56);
  for(let i=0;i<pn;i++){const at=po+i*ps;if(data.readUInt32LE(at)===1&&(data.readBigUInt64LE(at+48)<16384n||data.readBigUInt64LE(at+8)%16384n!==data.readBigUInt64LE(at+16)%16384n))throw new Error('Dependency fails 16KiB alignment: '+name);}
  dependencies.push({name,source:dependency,path:destination,size:data.length,sha256:sha(data)});
 }
}

const diff=cp.execFileSync('git',['-C',root,'diff','--binary','HEAD'],{maxBuffer:32*1024*1024,windowsHide:true});
const record={schemaVersion:2,createdAt:new Date().toISOString(),sourceCommit,sourceDiffSha256:sha(diff),sourceStatus:git(['status','--short']),packageDir,dependencies,packagedLibrary,packagedSize:packaged.length,packagedSha256:sha(packaged),library,size:elf.length,sha256:sha(elf),loads,ndk,gate:path.join(build,'orca_mobile_stl_gate')};
fs.writeFileSync(path.join(evidence,'native-build-manifest.json'),JSON.stringify(record,null,2)+String.fromCharCode(10));
fs.writeFileSync(path.join(evidence,'source.diff'),diff);
console.log(JSON.stringify(record,null,2));

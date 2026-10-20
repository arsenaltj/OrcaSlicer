'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {checkDocs,mobileFiles,platformFiles}=require('./verify-requirements.cjs');
function current(){const docs=path.resolve(__dirname,'..');return{md:fs.readFileSync(path.join(docs,'REQUIREMENTS-ANALYSIS.md'),'utf8'),html:fs.readFileSync(path.join(docs,'REQUIREMENTS-ANALYSIS.html'),'utf8'),topology:fs.readFileSync(path.join(docs,'PROJECT-TOPOLOGY.md'),'utf8'),mobilePaths:Object.fromEntries(mobileFiles.map(p=>[p,true])),platformPaths:Object.fromEntries(platformFiles.map(p=>[p,true]))};}
test('missing platform reference remains false instead of being forced true',()=>{const input=current();input.md=input.md.replaceAll('contracts/','');const checks=checkDocs(input);assert.equal(checks.report_uses_actual_platform_paths,false);assert.equal(Object.values(checks).every(Boolean),false);});
test('missing actual directory is reported even when document text looks correct',()=>{const input=current();input.platformPaths['src/modules']=false;const checks=checkDocs(input);assert.equal(checks.platform_actual_paths,false);});
test('TOC verification checks target IDs rather than the presence of the href itself',()=>{const input=current();input.html+='<a href="#missing-target">missing</a>';assert.equal(checkDocs(input).toc_targets,false);});

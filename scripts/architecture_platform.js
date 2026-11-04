// Inlined into the standalone explorer by architecture_review.py.
// Responsibilities are curated; dependency examples come from the selected source snapshot.
const platformCapabilities = Object.fromEntries((r.catalog?.capabilities || []).map(c => [c.id, c]));

function platformCard(c) {
  const button = btn('', origin => open({kind:'capability', id:c.id, label:c.label}, origin), 'platform-cap '+c.impact.kind);
  button.append(el('strong', c.label), el('span', c.files.length+' 个文件 · '+c.indexed_files+' 个已解析', 'platform-count'));
  return button;
}

function catalogFiles(host, paths) {
  const search = el('input');
  search.type = 'search'; search.className = 'search section';
  search.placeholder = '按路径筛选代码与测试'; search.setAttribute('aria-label', '筛选代码与测试');
  const list = el('div', undefined, 'files section');
  function draw() {
    const selected = paths.filter(p => p.toLowerCase().includes(search.value.toLowerCase()));
    list.replaceChildren(el('p', selected.length+' 个匹配文件；点击查看源码身份和锚点。', 'muted'));
    for (const p of selected) list.append(fileButton(p));
  }
  search.addEventListener('input', draw); host.append(search, list); draw();
}

function platformGuide() {
  const box = $('#inspector'), coverage = r.catalog.coverage;
  box.replaceChildren(el('div','PLATFORM / SOURCE SNAPSHOT','eyebrow'), el('h2','从职责定位到修改范围'),
    el('p','点击左侧能力，查看状态归属、契约、源码、验证入口，以及实际解析出的上下游。'),
    el('p','分层是逻辑职责。桌面中的多个能力仍共同编译进 libslic3r_gui；Python 本地 sidecar 是独立进程。'),
    el('p','DFX 横跨各层：诊断、日志、故障定位、性能与可靠性。当前入口分散，尚未形成统一平台。'));
  const counts = el('div',undefined,'counts');
  for (const [n,label] of [[coverage.inventory_files,'清单文件'],[coverage.indexed_files,'解析文件']]) {
    const d=el('div'); d.append(el('b',n),el('span',label)); counts.append(d);
  }
  box.append(counts, el('p','明确路径归属 '+coverage.curated_files+' 个；另有 '+coverage.directory_default_files+' 个按目录默认归属，尚未逐文件细分。'),
    el('div',r.mode==='PR 提交'?'源码绑定所标注提交；颜色只说明修改范围。':'当前为本地工作区快照，包含未提交的架构工具改动。','summary-note'),
    el('p','灰色不代表通过测试；静态关系会遗漏事件回调、跨进程协议和数据耦合。'));
}

function showRoot(host) {
  if (!r.catalog) { showBusinessRoot(host); return; }
  host.classList.add('platform-stage');
  host.append(title('PLATFORM ARCHITECTURE / 01', '先看平台职责，再进入代码',
    '五层逻辑架构。点击任意能力下钻；工作区、业务状态、契约、Orca 内核和运行保障分别说明。'));
  const actions=el('div',undefined,'platform-actions');
  actions.append(btn('AI 业务流程与状态 →', origin=>open({kind:'business',label:'AI 业务流程'},origin)),
    btn('覆盖范围与未归属文件', origin=>open({kind:'coverage',label:'覆盖范围'},origin)));
  host.append(actions);
  const stack=el('div',undefined,'platform-stack');
  r.catalog.layers.forEach((layer,index)=>{
    const row=el('section',undefined,'platform-layer layer-'+index),label=el('div',undefined,'platform-layer-label');
    label.append(el('span','0'+(index+1),'eyebrow'),el('h2',layer.label));
    const cards=el('div',undefined,'platform-cards');
    for (const id of layer.capabilities) cards.append(platformCard(platformCapabilities[id]));
    row.append(label,cards); stack.append(row);
  });
  host.append(stack);
  const dfx=btn('', origin=>open({kind:'capability',id:'dfx',label:platformCapabilities.dfx.label},origin),'dfx-band');
  dfx.append(el('strong','贯穿各层 · DFX'),el('span','诊断与日志 / 故障定位 / 性能基线 / 可靠性与恢复'));
  host.append(dfx);
  const scenarios=section('从需求／BUG 确定检查范围','业务建议清单，不是自动证明的完整影响集'),grid=el('div',undefined,'scenario-grid');
  for(const scenario of r.catalog.scenarios) grid.append(btn(scenario.label+' →', origin=>open({kind:'scenario',id:scenario.id,label:scenario.label},origin),'scenario-card'));
  scenarios.append(grid); host.append(scenarios,el('p','逻辑层允许同一文件承担多个职责；层的位置不代表禁止依赖规则已经实现。源码归属由 review-map.json 维护，依赖示例由静态解析补充。','reference-notes'));
}

function showPlatformCapability(host,id) {
  const c=platformCapabilities[id];
  host.append(title('CAPABILITY / 02',c.label,c.contract));
  const layout=el('div',undefined,'two'),owner=section('状态归属与边界'),proof=section('定位到当前源码');
  owner.append(el('h3','状态由谁维护'),el('p',c.state_owner),el('h3','实际构建／进程边界'),el('p',c.build_boundary),el('h3','当前限制与约束'),el('p',c.note));
  if(c.directory_default)owner.append(el('div','目录默认归属：用于完整导航，尚未逐文件细分业务职责。','summary-note'));
  proofButtons(c.evidence,proof); layout.append(owner,proof); host.append(layout);
  const links=section('观察到的代码依赖','箭头从使用方指向依赖方；计数是静态关系条目，非运行次数');
  const edges=r.catalog.dependencies.filter(e=>e.source===id||e.target===id);
  for(const edge of edges) {
    const other=platformCapabilities[edge.source===id?edge.target:edge.source];
    const row=el('details',undefined,'dependency-row');
    row.append(el('summary',platformCapabilities[edge.source].label+' → '+platformCapabilities[edge.target].label+' · '+edge.count+' 条'));
    row.append(btn('查看 '+other.label,origin=>open({kind:'capability',id:other.id,label:other.label},origin),'chip'));
    for(const example of edge.examples) {
      const item=el('div',undefined,'proof'); item.append(el('p',example.kind+' · '+(example.confidence||'静态证据'),'muted'),fileButton(example.source,example.line),el('span','依赖于','muted'),fileButton(example.target)); row.append(item);
    }
    links.append(row);
  }
  if(!edges.length) links.append(el('p','当前解析器未解析出跨能力边。请结合协议、事件和数据流复核。'));
  host.append(links);
  const tests=section('修改后优先检查','候选入口来自维护清单；此报告不运行测试');
  for(const path of c.test_files) tests.append(fileButton(path));
  if(!c.test_files.length) tests.append(el('p','尚未登记自动回归入口，需要在修改前确定验证方法。'));
  host.append(tests,section('该能力的全部代码文件',c.files.length+' 个文件 · '+c.lines+' 行 · '+c.indexed_files+' 个已解析'));
  catalogFiles(host,c.files); inspectImpact(c.label,c.impact);
}

function showScenario(host,id) {
  const scenario=r.catalog.scenarios.find(s=>s.id===id);
  host.append(title('CHANGE SCOPE / 02',scenario.label,'先确定谁拥有状态、谁产生候选、谁应用原生修改，再结合文件静态关系缩小检查范围。'));
  const cards=el('div',undefined,'scenario-grid');
  for(const key of scenario.capabilities) cards.append(platformCard(platformCapabilities[key]));
  host.append(cards,el('h2','相关验证'),el('p',scenario.verify,'summary-note'),
    el('p','这是维护的业务检查清单，涵盖部分静态图无法连接的数据和协议关系。不能据此省略修改实际影响的消费者。'));
  platformGuide();
}

function showCoverage(host) {
  const c=r.catalog,coverage=c.coverage;
  host.append(title('COVERAGE / 02','覆盖范围与未归属代码',
    '清单 '+coverage.inventory_files+' 个文件，静态解析 '+coverage.indexed_files+' 个，已归属 '+coverage.mapped_files+' 个。'));
  for(const line of c.limits) host.append(el('p',line));
  host.append(el('p','未进入符号解析的文件：'+coverage.unindexed_files+'；生产代码未归属：'+c.unassigned_files.length+'。测试或工程文件可能在清单内，但未归属某个产品能力。'),
    el('p','明确路径归属：'+coverage.curated_files+'；目录默认归属：'+coverage.directory_default_files+'。目录默认项只用于补齐导航，不证明其内部已解耦或完成业务审查。'),
    el('p','排除的目录由版本化 index_exclude 定义。清单不是全仓库文件计数；资源、依赖及非选定语言不在此统计中。'),
    el('p','快照清单 SHA-256 '+c.inventory_identity,'file'),section('尚未归属到能力的生产文件'));
  catalogFiles(host,c.unassigned_files); platformGuide();
}

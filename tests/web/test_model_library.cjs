const test = require("node:test");
const assert = require("node:assert/strict");
const {
  project,
  safeImage,
  supported,
  GeometryTransfer,
  retainLocalImages,
  statusLabel,
  accessibleVersions,
} = require("../../resources/web/model_library/library-state.js");
const local = [
  {
    key: "original",
    title: "本地",
    details: "GLB",
    generatedAt: 2,
    local: true,
  },
  {
    key: "download",
    title: "下载",
    details: "",
    local: true,
    sourceVerified: true,
    modelId: "m",
    versionId: "v",
    sha256: "a".repeat(64),
    generatedAt: 3,
  },
  { key: "edit", title: "改色", details: "", local: true, generatedAt: 4 },
  {
    key: "print",
    title: "打印",
    details: "",
    local: true,
    generatedAt: 5,
    printFeedback: "success",
  },
  {
    key: "image",
    title: "设计图",
    details: "",
    local: true,
    designOnly: true,
    generatedAt: 6,
  },
];
const cloud = [
  {
    id: "m",
    versionId: "v",
    title: "网站模型",
    createdAt: "2026-11-10T00:00:00Z",
    downloadCount: 3,
  },
];
test("owned website records use existing tags and only returned versions", () => {
  assert.equal(statusLabel({owned:true, status:"draft", visibility:"private"}), "草稿 · 私有");
  assert.equal(statusLabel({owned:true, status:"published", withdrawn:true}), "已撤回");
  assert.equal(statusLabel({status:"draft"}), "");
  const value = {owned:true,version:{id:"v"},versions:[{id:"v",status:"published"},{id:"d",status:"draft"}]};
  assert.deepEqual(accessibleVersions(value),value.versions);
  assert.deepEqual(accessibleVersions({...value,owned:false}),[value.versions[0]]);
  const items=project(local,[{...cloud[0],owned:true,status:"draft"}],"mine","models","","newest");
  assert.ok(items.find(x=>x.id==="m"));
  assert.ok(items.find(x=>x.key==="edit"));
  assert.equal(items.find(x=>x.key==="download"),undefined);
});
test("the same website version links its download while independent edits remain separate", () => {
  const values = project(local, cloud, "mine", "all", "", "newest");
  assert.equal(values.length, 5);
  assert.equal(values.find((x) => x.id === "m").linkedLocal.key, "download");
  assert.ok(values.find((x) => x.key === "edit"));
});
test("my models includes generated imported downloaded assets and leaves images in all", () => {
  const values = project(local, [], "mine", "models", "", "newest");
  assert.equal(values.length, 4);
  assert.equal(
    values.some((x) => x.designOnly),
    false,
  );
});
test("printing requires an existing real print result and does not infer it from a download", () => {
  assert.deepEqual(
    project(local, [], "mine", "prints", "", "newest").map((x) => x.key),
    ["print"],
  );
});
test("account filters cannot absorb unrelated local records", () => {
  for (const kind of ["likes", "favorites", "downloads", "views"])
    assert.equal(project(local, cloud, "mine", kind, "", "newest").length, 1);
  assert.equal(project(local, cloud, "library", "all", "", "newest").length, 1);
});
test("a changed version or missing digest cannot claim a local association", () => {
  for (const value of [
    { ...cloud[0], versionId: "other" },
    { ...cloud[0], id: "other" },
  ])
    assert.equal(
      project(local, [value], "mine", "all", "", "newest").find((x) => x.id)
        .linkedLocal,
      undefined,
    );
  assert.equal(
    project(
      local.map((x) => ({ ...x, sha256: "" })),
      cloud,
      "mine",
      "all",
      "",
      "newest",
    ).length,
    6,
  );
});
test("local searching and sorting preserve asset identities", () => {
  assert.deepEqual(
    project(local, [], "mine", "all", "改色", "title").map((x) => x.key),
    ["edit"],
  );
});
test("combined account records follow their real metric order without inventing local counts", () => {
  const records = [
    { ...cloud[0], id: "low", downloadCount: 1 },
    { ...cloud[0], id: "high", downloadCount: 9 },
    { ...cloud[0], id: "zero", downloadCount: 0 },
  ];
  const values = project(local, records, "mine", "all", "", "downloads");
  assert.deepEqual(
    values.slice(0, 3).map((x) => x.id),
    ["high", "low", "zero"],
  );
  assert.equal(values.find((x) => x.key === "edit").downloadCount, undefined);
});
test("query and account changes preserve local thumbnails while clearing website images", () => {
  const images = new Map([
    ["original", "local"],
    ["m:v", "private"],
    ["removed", "old"],
  ]);
  retainLocalImages(images, local);
  assert.deepEqual([...images], [["original", "local"]]);
  retainLocalImages(images, []);
  assert.equal(images.size, 0);
});
test("only native-provided data images can render and only native formats can be consumed", () => {
  assert.ok(safeImage("data:image/png;base64,YQ=="));
  for (const value of [
    "file:///secret",
    "https://platform.test/image",
    "javascript:alert(1)",
    "data:text/html;base64,YQ==",
  ])
    assert.equal(safeImage(value), false);
  for (const file of [
    "model.glb",
    "model.obj",
    "model.3mf",
    "model.stl",
    "model.step",
    "model.stp",
  ])
    assert.ok(supported(file));
  assert.equal(supported("model.exe"), false);
});

test("a changed local original remains separate even with unchanged source metadata", () => {
  const values = project(
    local.map((x) => ({ ...x, sourceVerified: false })),
    cloud,
    "mine",
    "all",
    "",
    "newest",
  );
  assert.equal(values.length, 6);
  assert.equal(values.find((x) => x.id === "m").linkedLocal, undefined);
});
test("geometry delivery rejects stale, missing and oversized chunks", () => {
  const transfer = new GeometryTransfer();
  assert.equal(transfer.begin("m:v", 3, 64 * 1024 * 1024 + 1), false);
  assert.ok(transfer.begin("m:v", 3, 20));
  const bytes = Buffer.alloc(20, 7);
  assert.equal(transfer.append("old:v", 2, 0, bytes.toString("base64")), false);
  assert.ok(
    transfer.append("m:v", 3, 0, bytes.subarray(0, 10).toString("base64")),
  );
  assert.equal(transfer.finish("m:v", 3), null);
  assert.ok(
    transfer.append("m:v", 3, 10, bytes.subarray(10).toString("base64")),
  );
  assert.deepEqual(Buffer.from(transfer.finish("m:v", 3)), bytes);
  transfer.begin("m:v", 4, 20);
  assert.equal(transfer.append("m:v", 4, 1, "AA=="), false);
  assert.equal(transfer.finish("m:v", 4), null);
});


test("model library batches commit one browser frame and preserve invalidation during render", () => {
  const {FrameUpdates} = require("../../resources/web/model_library/library-state.js");
  const queue = [];
  let commits = 0;
  const updates = new FrameUpdates(fn => queue.push(fn), () => {
    ++commits;
    if (commits === 1) updates.request();
  });
  updates.batch(() => {
    for (let i=0; i<20; ++i) updates.request();
    updates.batch(() => updates.request());
    assert.equal(queue.length, 0);
  });
  updates.batch(() => updates.request());
  assert.equal(queue.length, 1);
  queue.shift()();
  assert.equal(commits, 1);
  assert.equal(queue.length, 1);
  queue.shift()();
  assert.equal(commits, 2);
  updates.request();
  updates.dispose();
  queue.shift()();
  assert.equal(commits, 2);
});

test("failed model library batches still release their pending render", () => {
  const {FrameUpdates} = require("../../resources/web/model_library/library-state.js");
  const queue = [];
  let commits = 0;
  const updates = new FrameUpdates(fn => queue.push(fn), () => ++commits);
  assert.throws(() => updates.batch(() => { updates.request(); throw new Error("bad message"); }));
  assert.equal(queue.length, 1);
  queue.shift()();
  assert.equal(commits, 1);
});

test("a rejected frame post preserves changes for the next render request", () => {
  const {FrameUpdates} = require("../../resources/web/model_library/library-state.js");
  const queue = [];
  let reject = true, commits = 0;
  const updates = new FrameUpdates(callback => {
    if (reject) throw new Error("frame post rejected");
    queue.push(callback);
  }, () => ++commits);
  assert.throws(() => updates.request(), /frame post rejected/);
  reject = false;
  updates.request();
  updates.request();
  assert.equal(queue.length, 1);
  queue.shift()();
  assert.equal(commits, 1);
});

// Execute the installed script and its real event handlers. This small DOM
// adapter supplies elements only; it does not duplicate page/bridge behavior.
function installedPage() {
  const fs = require("node:fs"), path = require("node:path"), vm = require("node:vm");
  class Element {
    constructor(tag = "div") {
      this.tagName = tag; this.children = []; this.dataset = {}; this.style = {};
      this.attributes = {}; this.hidden = false; this.className = ""; this.value = "";
      this.classList = { toggle: (name, active) => {
        const names = new Set(this.className.split(/\s+/).filter(Boolean));
        if (active) names.add(name); else names.delete(name);
        this.className = [...names].join(" ");
      }};
    }
    set textContent(text) { this.children = []; this.text = String(text); }
    get textContent() { return (this.text || "") + this.children.map(x => x.textContent ?? String(x)).join(""); }
    append(...values) { for (const value of values) { if (typeof value === "object") value.parent = this; this.children.push(value); } }
    prepend(value) { value.parent = this; this.children.unshift(value); }
    replaceChildren(...values) { this.children = []; this.text = ""; this.append(...values); }
    setAttribute(key, value) { this.attributes[key] = String(value); }
    getAttribute(key) { return this.attributes[key] ?? null; }
    removeAttribute(key) { delete this.attributes[key]; }
    contains(value) { return value === this || this.children.some(x => x.contains?.(value)); }
    querySelector(selector) {
      for (const value of this.children) {
        if (typeof value !== "object") continue;
        if (selector.startsWith(".") ? value.className?.split(" ").includes(selector.slice(1)) : value.tagName === selector) return value;
        const nested = value.querySelector?.(selector); if (nested) return nested;
      }
      return null;
    }
    remove() { if (this.parent) this.parent.children = this.parent.children.filter(x => x !== this); }
    getBoundingClientRect() { return {right: 240, bottom: 180}; }
    focus() {}
  }
  const directory = path.resolve(__dirname, "../../resources/web/model_library");
  const html = fs.readFileSync(path.join(directory, "index.html"), "utf8");
  const ids = new Map([...html.matchAll(/id="([^"]+)"/g)].map(x => [x[1], new Element()]));
  const scopes = ["mine", "library"].map(scope => { const node = new Element("button"); node.dataset.scope = scope; return node; });
  const frames = [], timers = [], sent = [];
  const window = { libraryState: require("../../resources/web/model_library/library-state.js"),
    wx: {postMessage: text => sent.push(JSON.parse(text))}, libraryViewer: { close() {} } };
  window.requestAnimationFrame = function(callback) {
    assert.equal(this, window, "the browser frame API requires its Window receiver");
    frames.push(callback);
  };
  const document = {getElementById: id => { assert.ok(ids.has(id), `installed id ${id}`); return ids.get(id); },
    createElement: tag => new Element(tag), createTextNode: text => ({textContent: text}), addEventListener() {},
    querySelectorAll: selector => selector === "[data-scope]" ? scopes : []};
  vm.runInNewContext(fs.readFileSync(path.join(directory, "library.js"), "utf8"), {
    window, document, innerWidth: 1440, innerHeight: 860,
    requestAnimationFrame: window.requestAnimationFrame,
    setTimeout: callback => { timers.push(callback); return timers.length; }, clearTimeout() {},
  }, {filename: "installed-library.js"});
  let doc = 1, account = "anonymous", generation = 0;
  const receive = value => window.modelLibraryReceive({documentGeneration: doc, accountTag: account,
    accountGeneration: generation, requestGeneration: sent.at(-1)?.requestGeneration || 0, ...value});
  function handshake(nextDoc = 1) {
    doc = nextDoc;
    window.modelLibraryReceive({type: "init", nonce: `nonce-${doc}`, documentGeneration: doc});
    assert.equal(sent.at(-1).command, "ready");
  }
  function login(tag = "account", nextGeneration = 1, avatarData = "") {
    account = tag; generation = nextGeneration;
    receive({type: "account", signedIn: tag !== "anonymous", name: "用户", projectAvailable: true, avatarData});
  }
  function flush() { let budget = 20; while (frames.length && budget--) frames.shift()(); assert.ok(budget > 0); }
  return {ids, scopes, sent, receive, handshake, login, flush};
}

const scriptCard = {id: "model-one", versionId: "version-one", version: 1,
  title: "真实模型", category: "", author: {displayName: "作者"}};
function scriptDetail() {
  return { ...scriptCard, publicInteraction: true, description: "模型说明", author: {displayName: "作者"},
    version: {id: "version-one", version: 1, licenseName: "CC0", licenseDescription: "许可说明"},
    versions: [{id: "version-one", version: 1, status: "published"}], access: {allowed: true},
    files: [{id: "file-one", filename: "model.glb", size: 8}] };
}

test("installed page renders its first empty state and then a populated catalog in browser frames", () => {
  const page = installedPage();
  page.handshake();page.login("anonymous", 0);page.flush();
  assert.equal(page.ids.get("empty").hidden, false);
  assert.equal(page.ids.get("emptyText").textContent, "暂无模型");
  page.receive({type: "taxonomy", value: {categories: [{slug: "art", label: "艺术与设计"}]}});
  page.receive({type: "catalog", items: [scriptCard], replace: true, hasMore: false});page.flush();
  assert.match(page.ids.get("filters").textContent, /艺术与设计/);
  assert.equal(page.ids.get("empty").hidden, true);
  assert.equal(page.ids.get("grid").children.length, 1);
});

test("installed page handshakes before rendering public and owned catalogs and keeps successful lists after errors", () => {
  const page = installedPage();page.handshake();page.login("anonymous", 0);
  page.receive({type: "catalog", items: [scriptCard], replace: true, hasMore: false});page.flush();
  assert.equal(page.ids.get("grid").children.length, 1);
  assert.match(page.ids.get("grid").textContent, /真实模型/);
  page.receive({type: "error", message: "分类暂不可用"});page.flush();
  assert.equal(page.ids.get("grid").children.length, 1);
  page.login();page.scopes[0].onclick();
  assert.equal(page.sent.at(-1).scope, "mine");
  const query = page.sent.at(-1);
  page.receive({type: "catalog", items: [{...scriptCard, owned: true, status: "draft", visibility: "private"}], replace: true});page.flush();
  assert.match(page.ids.get("grid").textContent, /草稿 · 私有/);
  page.receive({type: "catalog", requestGeneration: query.requestGeneration - 1, items: [], replace: true});page.flush();
  assert.equal(page.ids.get("grid").children.length, 1);
  page.receive({type: "account", accountTag: "obsolete", accountGeneration: 0});page.flush();
  assert.equal(page.ids.get("grid").children.length, 1);
});

test("installed download and cancel buttons send exact identities and always recover their busy state", () => {
  const page=installedPage();page.handshake();page.login();
  page.receive({type: "catalog", items: [scriptCard], replace: true});page.flush();
  page.ids.get("grid").querySelector("button").onclick();
  assert.equal(page.sent.at(-1).command, "detail");
  assert.equal(page.sent.at(-1).key, "model-one:version-one");
  page.receive({type: "detail", key: "model-one:version-one", value: scriptDetail()});
  page.ids.get("files").querySelector("button").onclick();
  const download=page.sent.at(-1);assert.equal(download.command, "download");assert.equal(download.file, "file-one");
  assert.equal(download.nonce, "nonce-1");assert.equal(download.accountTag, "account");
  page.receive({type: "loading", active: true, cancelable: true, stage: "正在下载并校验模型…"});
  assert.equal(page.ids.get("app").inert, true);
  page.ids.get("cancel").onclick();assert.equal(page.sent.at(-1).command, "cancel");
  page.receive({type: "loading", active: true, requestGeneration: download.requestGeneration, cancelable: true});
  page.receive({type: "loading", active: false});assert.equal(page.ids.get("app").inert, false);
  for (const message of ["下载队列暂不可用", "登录已失效", "当前账号没有下载资格", "文件已下架", "网络连接中断", "文件校验失败"]) {
    page.receive({type: "loading", active: true, cancelable: true});
    page.receive({type: "error", message});assert.equal(page.ids.get("app").inert, false);
    assert.equal(page.ids.get("loading").hidden, true);assert.match(page.ids.get("notice").textContent, new RegExp(message));
  }
  page.receive({type: "loading", active: true, cancelable: true});page.login("anonymous", 2);
  assert.equal(page.ids.get("app").inert, false);
  page.receive({type: "detail", value: scriptDetail(), key: "model-one:version-one"});
  page.ids.get("files").querySelector("button").onclick();
  assert.equal(page.sent.at(-1).file, "file-one"); // Preserve selection across native login.
  page.handshake(2);page.login("anonymous", 2);
  page.receive({type: "catalog", documentGeneration: 1, items: [scriptCard], replace: true});page.flush();
  assert.equal(page.ids.get("grid").children.length, 0);
});

test("local card menus expose only opening and server-confirmed direct slicing", () => {
  const cases = [
    {key: "native", extension: ".stl", canDirectImport: true, labels: ["打开模型", "导入切片"]},
    {key: "workbench", extension: ".obj", canDirectImport: false, labels: ["打开模型"]},
    {key: "unknown", extension: ".stl", labels: ["打开模型"]},
    {key: "design", designOnly: true, labels: ["打开设计图"]},
  ];
  for (const {labels, ...asset} of cases) {
    const page = installedPage();
    page.handshake();
    page.login();
    page.scopes[0].onclick();
    page.receive({type: "local", revision: 15, items: [{
      title: asset.key, local: true, imported: true, generatedAt: 1, ...asset,
    }]});
    page.flush();
    const more = page.ids.get("grid").querySelector(".menu-button");
    const menu = page.ids.get("menu");
    more.onclick({stopPropagation() {}});
    assert.deepEqual(menu.children.map(button => button.textContent), labels);
    menu.children[0].onclick();
    assert.equal(menu.hidden, true);
    const opened = page.sent.at(-1);
    assert.deepEqual([opened.command, opened.action, opened.key, opened.revision], ["local", "open", asset.key, 15]);
    if (asset.canDirectImport) {
      more.onclick({stopPropagation() {}});
      menu.children[1].onclick();
      const imported = page.sent.at(-1);
      assert.deepEqual([imported.command, imported.action, imported.key, imported.revision], ["local", "import", asset.key, 15]);
    }
  }
});

test("account avatar changes update only local and owned card images in place", () => {
  const page = installedPage();
  const first = "data:image/png;base64,AAAA";
  const next = "data:image/png;base64,AQID";
  const author = "data:image/png;base64,BAUG";
  const guest = "data:image/png;base64,BwgJ";
  page.handshake();
  page.login("account", 1, first);
  page.scopes[0].onclick();
  page.receive({type: "local", revision: 2, items: [
    {key: "shape", title: "本地模型", local: true, generatedAt: 1},
    {key: "design", title: "设计图", local: true, designOnly: true, generatedAt: 2},
  ]});
  page.receive({type: "catalog", items: [
    {...scriptCard, title: "自有作品", owned: true, author: {displayName: "我"}},
    {...scriptCard, id: "by-other", title: "收藏作品", author: {displayName: "作者", avatarUrl: author}},
  ], replace: true});
  page.flush();
  const grid = page.ids.get("grid");
  const card = title => grid.children.find(node => node.querySelector(".name").textContent === title);
  const avatarOf = node => node.querySelector(".avatar").querySelector("img");
  const owned = [card("本地模型"), card("设计图"), card("自有作品")];
  const other = card("收藏作品");
  const otherImage = avatarOf(other);
  assert.equal(otherImage.src, author);
  for (const node of owned) assert.equal(avatarOf(node).src, first);

  page.login("account", 1, next);
  page.flush();
  for (const node of owned) {
    assert.ok(grid.children.includes(node));
    assert.equal(avatarOf(node).src, next);
  }
  assert.equal(avatarOf(other), otherImage);

  page.login("anonymous", 2, guest);
  page.receive({type: "account", accountGeneration: 1, accountTag: "account", signedIn: true, avatarData: next});
  page.flush();
  assert.equal(avatarOf(card("本地模型")).src, guest);
  assert.equal(avatarOf(card("设计图")).src, guest);
  assert.equal(card("自有作品"), undefined);
});

test("public card menus always open the selected version's details", () => {
  const page = installedPage();
  page.handshake();
  page.login("account", 1, "data:image/png;base64,AAAA");
  page.receive({type: "local", revision: 3, items: [{
    key: "download", title: "本地副本", local: true, extension: ".3mf", canDirectImport: true,
    modelId: scriptCard.id, versionId: scriptCard.versionId, sourceVerified: true, sha256: "a".repeat(64),
  }]});
  const authorAvatar = "data:image/png;base64,AQID";
  const firstVersion = {...scriptCard, author: {...scriptCard.author, avatarUrl: authorAvatar}};
  const otherVersion = {...firstVersion, version: 2, versionId: "version-two"};
  page.receive({type: "catalog", items: [firstVersion, otherVersion], replace: true});
  page.flush();
  const [downloaded, publicCard] = page.ids.get("grid").children;
  assert.match(downloaded.textContent, /已下载/);
  assert.doesNotMatch(publicCard.textContent, /已下载/);
  const menu = page.ids.get("menu");
  for (const [card, versionId] of [[downloaded, scriptCard.versionId], [publicCard, otherVersion.versionId]]) {
    assert.equal(card.querySelector(".avatar").querySelector("img").src, authorAvatar);
    card.querySelector(".menu-button").onclick({stopPropagation() {}});
    assert.deepEqual(menu.children.map(button => button.textContent), ["模型详情"]);
    menu.children[0].onclick();
    assert.equal(menu.hidden, true);
    assert.equal(page.sent.at(-1).command, "detail");
    assert.equal(page.sent.at(-1).key, `${scriptCard.id}:${versionId}`);
  }
});

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

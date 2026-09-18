"""PR architecture impact: Git truth + bounded code-review-graph analysis.

No application code is imported or executed. Outputs contain paths/relationships,
not source text. The graph is advisory; uncovered files remain visible.
"""
from __future__ import annotations

import argparse
import ast
from collections import Counter
import hashlib
import importlib.metadata
import json
import os
import posixpath
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

VERSION = "2"
ENGINE = "2.3.8"
EXTENSIONS = {".cpp", ".hpp", ".h", ".c", ".cc", ".py"}
LOCK = "docs/architecture/ai-integration-lock.json"
MAP = "Docs/architecture/review-map.json"
ARCHIFY_VERSION = "2.16.0"
ARCHIFY_URL = f"https://github.com/tt-a1i/archify/releases/download/v{ARCHIFY_VERSION}/archify.zip"
ARCHIFY_SHA256 = "4c59fa6557a2385beaaef8c7219cc414573acc9f0c30a932d5053b0b20689a46"


def archify_home(root):
    return root / ".tmp/architecture-review" / f"archify-{ARCHIFY_VERSION}" / "archify"


def setup_archify(root):
    """Explicit one-time setup; normal report generation never downloads tools."""
    folder = root / ".tmp/architecture-review"
    folder.mkdir(parents=True, exist_ok=True)
    archive = folder / f"archify-{ARCHIFY_VERSION}.zip"
    if not archive.exists():
        with urllib.request.urlopen(ARCHIFY_URL, timeout=120) as response:
            data = response.read(32 * 1024 * 1024 + 1)
        if digest(data) != ARCHIFY_SHA256:
            raise RuntimeError("Official Archify archive checksum mismatch")
        archive.write_bytes(data)
    if digest(archive.read_bytes()) != ARCHIFY_SHA256:
        raise RuntimeError("Local Archify archive checksum mismatch; do not execute it")
    destination = archify_home(root).parent.resolve()
    if not destination.is_relative_to(root / ".tmp"):
        raise ValueError("Archify tools must stay inside this checkout's .tmp")
    with zipfile.ZipFile(archive) as bundle:
        for member in bundle.infolist():
            path = valid_path(member.filename.rstrip("/"))
            if not path.startswith("archify/") or not (destination / path).resolve().is_relative_to(destination):
                raise ValueError("Unsafe Archify archive entry")
            if (member.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("Archify archive must not contain symlinks")
        bundle.extractall(destination)
    print(json.dumps({"status": "ARCHIFY_READY", "version": ARCHIFY_VERSION,
                      "archive_sha256": ARCHIFY_SHA256, "path": str(archify_home(root))}))


def git(root, *args):
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def digest(value):
    return hashlib.sha256(value).hexdigest()


def valid_path(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or "\\" in value or ":" in value or ".git" in path.parts:
        raise ValueError(f"Unsafe repository path: {value!r}")
    return value


def matches(path, prefix):
    return path == prefix or path.startswith(prefix + "/") or (prefix.endswith("_") and path.startswith(prefix))


def tree(root, ref):
    entries = {}
    for row in git(root, "ls-tree", "-rz", ref).split(b"\0"):
        if row:
            meta, name = row.split(b"\t", 1)
            mode, kind, oid = meta.decode().split()
            path = name.decode("utf-8")
            if kind == "blob":
                entries[valid_path(path)] = (mode, oid)
    return entries


def read_blobs(root, entries, paths):
    paths = sorted(paths)
    if not paths:
        return {}
    request = "".join(entries[p][1] + "\n" for p in paths).encode()
    proc = subprocess.run(["git", "-C", str(root), "cat-file", "--batch"], input=request, capture_output=True, timeout=120)
    if proc.returncode:
        raise RuntimeError("Could not read source blobs")
    data, pos, blobs = proc.stdout, 0, {}
    for path in paths:
        end = data.index(b"\n", pos)
        _, kind, size = data[pos:end].split()
        if kind != b"blob":
            raise RuntimeError("Expected a Git blob")
        start = end + 1
        pos = start + int(size)
        blobs[path] = data[start:pos]
        pos += 1
    return blobs


def change_list(root, base, head, worktree=False):
    args = ["diff", "--name-status", "-z", "--no-renames", base]
    if not worktree:
        args.append(head)
    parts = git(root, *args, "--").split(b"\0")
    changes = [{"status": parts[i].decode(), "path": valid_path(parts[i + 1].decode("utf-8"))}
               for i in range(0, len(parts) - 1, 2)]
    if worktree:
        tracked = {c["path"] for c in changes}
        changes.extend({"status": "A", "path": valid_path(p.decode("utf-8"))}
                       for p in git(root, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
                       if p and p.decode("utf-8") not in tracked)
    return sorted(changes, key=lambda c: c["path"])


def load_map(root):
    config_bytes = (root / MAP).read_bytes()
    lock_bytes = (root / LOCK).read_bytes()
    config, lock = json.loads(config_bytes), json.loads(lock_bytes)
    ids = set()
    for module in config["modules"]:
        if not re.fullmatch(r"[a-z][a-z0-9_]*", module["id"]) or module["id"] in ids:
            raise ValueError("Invalid/duplicate module id")
        if not re.fullmatch(r"[\w \u4e00-\u9fff]+", module["label"]):
            raise ValueError("Invalid module label")
        ids.add(module["id"])
        module["paths"] += [p for key in module["ownership"] for p in lock["boundaries"][key]]
        for path in module["paths"]:
            valid_path(path)
    for path in config["index_scope"]:
        valid_path(path)
    config["modules"].append({"id": "unknown", "label": "待归属", "paths": []})
    config["identity"] = digest(config_bytes + lock_bytes)
    return config


def module_for(path, config):
    # Test files are a separate view even inside a provider's owned directory.
    if PurePosixPath(path).name.startswith("test_"):
        return "tests"
    candidates = [(len(prefix), m["id"]) for m in config["modules"] for prefix in m["paths"] if matches(path, prefix)]
    return max(candidates)[1] if candidates else "unknown"


def indexed(path, config):
    return PurePosixPath(path).suffix in EXTENSIONS and any(matches(path, p) for p in config["index_scope"])


def sources(root, entries, config, worktree=False):
    paths = {p for p, (mode, _) in entries.items() if mode in {"100644", "100755"} and indexed(p, config)}
    if not worktree:
        return read_blobs(root, entries, paths)
    paths.update(p.decode("utf-8") for p in git(root, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0") if p)
    result = {}
    for p in sorted(paths):
        valid_path(p)
        file = root / p
        if indexed(p, config) and file.is_file() and not file.is_symlink() and file.resolve().is_relative_to(root):
            result[p] = file.read_bytes()
    return result


def localize(value, stage):
    """Strip temporary snapshot locations from graph facts and cached records."""
    if isinstance(value, str):
        return value.replace(stage.as_posix() + "/", "").replace(str(stage) + os.sep, "")
    if isinstance(value, list):
        return [localize(v, stage) for v in value]
    if isinstance(value, dict):
        return {k: localize(v, stage) for k, v in value.items()}
    return value


def include_target(source, target, files):
    """Conservative path inference; never pick among duplicate header names."""
    relative = posixpath.normpath(posixpath.join(posixpath.dirname(source), target))
    if relative in files:
        return relative
    candidates = [p for p in files if p == target or p.endswith("/" + target)]
    return candidates[0] if len(candidates) == 1 else None


def analyze(blobs, config, cache):
    """Cache immutable graphs by exact source/config/engine inputs.

    CRG parses bytes, stores/resolves symbols, then supplies its dependency edges.
    No embeddings, server, installed hooks or application imports are involved.
    """
    from code_review_graph.parser import CodeParser
    from code_review_graph.graph import GraphStore
    from code_review_graph.python_resolver import resolve_python_imports
    from code_review_graph.scoped_resolver import resolve_scoped_calls
    from tree_sitter_language_pack import get_parser

    versions = {p: importlib.metadata.version(p) for p in ("code-review-graph", "tree-sitter", "tree-sitter-language-pack", "networkx")}
    if versions["code-review-graph"] != ENGINE:
        raise RuntimeError(f"Expected code-review-graph {ENGINE}; install the pinned requirements")
    # CRG can degrade to file-only nodes when a native grammar cannot load.
    # Fail explicitly instead of reporting an empty dependency graph as success.
    for language in {"python" if p.endswith(".py") else "cpp" for p in blobs}:
        get_parser(language)
    hashes = {p: digest(data) for p, data in sorted(blobs.items())}
    identity = digest(json.dumps([VERSION, digest(Path(__file__).read_bytes()), versions, config["identity"], hashes], sort_keys=True).encode())
    cached = cache / (identity + ".json")
    if cached.exists():
        result = json.loads(cached.read_text(encoding="utf-8"))
        result["cache_hit"] = True
        return result
    cache.mkdir(parents=True, exist_ok=True)
    # Only this tool-owned temporary snapshot is cleaned up, never the checkout.
    with tempfile.TemporaryDirectory(prefix="snapshot-", dir=cache) as directory:
        stage = Path(directory).resolve()
        assert stage.is_relative_to(cache.resolve())
        for p, data in blobs.items():
            target = stage / valid_path(p)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        parser = CodeParser(stage)
        with GraphStore(stage / "graph.sqlite") as store:
            parsed = []
            for p, data in sorted(blobs.items()):
                nodes, edges = parser.parse_bytes(stage / p, data)
                if not nodes:
                    raise RuntimeError(f"Parser returned no nodes: {p}")
                store.store_file_nodes_edges(str(stage / p), nodes, edges, hashes[p])
                parsed.append(p)
            resolve_python_imports(store)
            store.resolve_bare_call_targets()
            store.resolve_cpp_scoped_call_targets()
            resolve_scoped_calls(store)
            all_nodes = [n for p in parsed for n in store.get_nodes_by_file(str(stage / p))]
            by_name = {n.qualified_name: n for n in all_nodes}
            file_nodes = {localize(n.file_path, stage): n for n in all_nodes if n.kind == "File"}
            facts, unresolved = [], 0
            for edge in store.get_all_edges():
                if edge.kind == "CONTAINS":
                    continue
                source, target = by_name.get(edge.source_qualified), by_name.get(edge.target_qualified)
                confidence = edge.confidence_tier
                if source and not target and edge.kind == "IMPORTS_FROM" and source.language in {"cpp", "c"}:
                    inferred = include_target(localize(source.file_path, stage), edge.target_qualified, file_nodes)
                    target = file_nodes.get(inferred)
                    confidence = "PATH_INFERRED"
                if not source or not target:
                    unresolved += 1
                    continue
                if source.file_path == target.file_path:
                    continue
                facts.append(localize({"source": source.file_path, "target": target.file_path,
                                       "kind": edge.kind, "line": edge.line,
                                       "confidence": confidence}, stage))
            result = {"identity": identity, "versions": versions, "files": parsed,
                      "node_count": len(all_nodes), "edges": facts, "unresolved_edges": unresolved,
                      "cache_hit": False}
    cached.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
    return result


def impact(graph, changed, depth=2):
    """Walk CRG dependency facts backwards; TESTED_BY points toward the test."""
    touched, frontier = set(changed), set(changed)
    for _ in range(depth):
        found = set()
        for edge in graph["edges"]:
            if edge["kind"] == "TESTED_BY":
                if edge["source"] in frontier:
                    found.add(edge["target"])
            elif edge["target"] in frontier:
                found.add(edge["source"])
        frontier = found - touched
        touched.update(frontier)
    return touched - set(changed)


def relationships(graph, config):
    return {(module_for(e["source"], config), module_for(e["target"], config))
            for e in graph["edges"] if e["kind"] != "TESTED_BY" and module_for(e["source"], config) != module_for(e["target"], config)}


def make_report(config, before, after, changes, metadata):
    changed = {c["path"] for c in changes}
    potential = (impact(before, changed) | impact(after, changed)) - changed
    old_edges, new_edges = relationships(before, config), relationships(after, config)
    counts = Counter(module_for(p, config) for p in changed)
    affected = Counter(module_for(p, config) for p in potential)
    modules = [{"id": m["id"], "label": m["label"], "purpose": m.get("purpose", "模块归属待补充"), "changed": counts[m["id"]], "potential": affected[m["id"]]} for m in config["modules"]]
    evidence = [{**e, "version": name} for name, graph in [("base", before), ("head", after)] for e in graph["edges"]
                if e["source"] in changed | potential and e["target"] in changed | potential]
    return {"schema": 1, **metadata, "modules": modules,
            "changes": [{**c, "module": module_for(c["path"], config), "indexed": c["path"] in set(before["files"]) | set(after["files"])} for c in changes],
            "potential_files": sorted(potential),
            "potential_by_module": {m["id"]: sorted(p for p in potential if module_for(p, config) == m["id"]) for m in config["modules"]},
            "evidence": evidence[:1000], "evidence_omitted": max(0, len(evidence) - 1000),
            "relationships": [{"source": a, "target": b, "status": "added" if (a,b) not in old_edges else "removed" if (a,b) not in new_edges else "existing"} for a,b in sorted(old_edges | new_edges)],
            "graph": {"engine": ENGINE, "versions": after.get("versions", {}), "before_files": len(before["files"]), "after_files": len(after["files"]),
                      "nodes": after["node_count"], "resolved_cross_file_edges": len(after["edges"]),
                      "unresolved_edges": after["unresolved_edges"], "cache_hits": int(before["cache_hit"]) + int(after["cache_hit"]),
                      "before_identity": before["identity"], "after_identity": after["identity"]},
            "limits": ["试点仅解析 AI、sidecar 与相关测试；其他文件只标记模块，不推断其代码影响。",
                       "潜在影响最多沿静态依赖反向追踪两跳，同时保留基线中的删除关系。",
                       "C++ include 按相对路径或唯一后缀推测；重复头文件不强行连接。未解析端点含外部库。",
                       "wxWidgets 事件、动态调用及 HTTP 跨进程关系可能遗漏；图不代替代码审查或测试。",
                       "红色表示关系移除，不表示失败；绿色表示新增关系，不表示验证通过。"]}


def mermaid(report):
    lines = ["flowchart LR"]
    for m in report["modules"]:
        lines.append(f'  {m["id"]}["{m["label"]} · 修改 {m["changed"]} / 关联 {m["potential"]}"]')
    for e in report["relationships"]:
        label = {"added": "新增", "removed": "移除", "existing": "依赖"}[e["status"]]
        lines.append(f'  {e["source"]} -->|{label}| {e["target"]}')
    lines += ["  classDef changed fill:#fff0c2,stroke:#b87800,color:#332600", "  classDef potential fill:#e3ecff,stroke:#4678cf,color:#162e55"]
    for m in report["modules"]:
        if m["changed"] or m["potential"]:
            lines.append(f'  class {m["id"]} {"changed" if m["changed"] else "potential"}')
    return "\n".join(lines)


def markdown(report):
    outside = sum(not c["indexed"] for c in report["changes"])
    return ("<!-- orca-architecture-impact -->\n### 架构影响图\n\n"
            f'版本：`{report["base"][:12]}` → `{report["head"][:12]}`；模式：{report["mode"]}。\n\n'
            f'直接修改 **{len(report["changes"])}** 个文件，潜在关联 **{len(report["potential_files"])}** 个文件；'
            f'其中 **{outside}** 个改动文件未做代码关系解析。\n\n'
            "黄色＝直接修改，蓝色＝潜在关联；箭头由使用方指向依赖方。\n\n```mermaid\n" + mermaid(report) + "\n```\n\n" +
            "\n".join("- " + line for line in report["limits"]) +
            "\n\n下载本次 Actions 的 `architecture-impact` 制品，解压后打开 `index.html`："
            "L0 产品全貌 → L1 模块职责 → L2 业务状态 → L3 源码改动。"
            "`details.html` 保留完整静态关系，`report.json` 供 AI 阅读。"
            "Archify 总览展示选定主干；业务语义为维护的说明，状态名称从源码提取。"
            "这里没有实时运行或业务验收状态，布局校验不等于代码验证。\n")


def render_html(report):
    return render_page("architecture_review.html", report)


README_START = "<!-- architecture-overview:start -->"
README_END = "<!-- architecture-overview:end -->"


def readme_overview(config):
    """Versioned product map, deliberately independent of a PR SHA or diff.

    Per-PR evidence lives in the report/comment. Including its own commit SHA
    here would create a self-changing output and force unrelated README churn.
    """
    def label(value):
        return str(value).replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;").replace(">", "&gt;").replace("|", "&#124;").replace("\n", " ")

    lines = [README_START, "## AI 产品架构与 PR 变动", "",
             "[团队使用说明](Docs/coordination/architecture-review.md) · "
             "[PR 架构报告](https://github.com/arsenaltj/OrcaSlicer/actions/workflows/architecture-impact.yml?query=event%3Apull_request) · "
             "[开放 PR](https://github.com/arsenaltj/OrcaSlicer/pulls)", "",
             "每次 PR 自动生成改动高亮图：在 PR 的机器人评论查看摘要，或在 Actions 下载 "
             "`architecture-impact`，解压打开 `index.html`，同页展开业务 → 状态 → 代码。", "",
             "下图是本分支维护的业务主线，不是实时状态或验收结果；箭头表示业务衔接，执行仍受用户确认与状态约束。", "",
             "```mermaid", "flowchart TB"]
    phase_nodes = {}
    for index, journey in enumerate(config["journeys"]):
        lines.append(f'  subgraph j{index}["{label(journey["label"])}"]')
        nodes = []
        for number, phase in enumerate(journey["phases"]):
            node = f"j{index}p{number}"
            nodes.append(node)
            phase_nodes[journey["id"], phase["id"]] = node
            lines.append(f'    {node}["{label(phase["label"])}"]')
        lines.append("    " + " --> ".join(nodes))
        lines.append("  end")
    overview = config["product_overview"]
    lines.append('  subgraph native["Orca 原生能力"]')
    for number, entry in enumerate(overview["native"]):
        lines.append(f'    n{number}["{label(entry["label"])}"]')
    lines += ["    " + " --> ".join(f"n{i}" for i in range(len(overview["native"]))), "  end"]
    # These are the same explicit business boundaries shown on the product canvas.
    lines += [f'  {phase_nodes["generation", "import"]} -->|用户确认导入| n0',
              f'  n1 -->|工作区与约束| {phase_nodes["slicing", "inspect"]}',
              f'  {phase_nodes["slicing", "decision"]} -->|明确应用后正式切片| n2',
              f'  n2 -->|完成回传| {phase_nodes["slicing", "commit"]}', "```", "",
              "业务关系（含规划）：", "", "| 输入 / 来源 | 输出 / 目标 | 衔接与条件 |", "| --- | --- | --- |"]
    for relation in overview["relations"]:
        status = "未做：" if relation.get("planned") else ""
        lines.append(f'| {label(relation["source"])} | {label(relation["target"])} | {status}{label(relation["detail"])} |')
    lines += ["", "目标能力占位（完整目标尚未完成；已有部分实现见交互报告）：", "", "| 分区 | 未做 |", "| --- | --- |"]
    groups = {"services": "服务与编排", "generation": "模型生成", "slicing": "智能切片", "foundation": "平台底座"}
    for group, entries in overview["planned"].items():
        lines.append(f'| {groups.get(group, group)} | {"、".join(label(item["label"]) + "（" + label(item["status"]) + "）" for item in entries)} |')
    lines += ["", "来源：`Docs/architecture/review-map.json`。业务/状态/边界改变时维护映射，运行 "
              "`python scripts/architecture_review.py --readme update` 更新此区域（仅需 Python 标准库）；"
              "Windows 的 `./dev.ps1 Review` 也会同步更新。CI 检查是否过期。", "",
              "README 展示当前分支的架构，合并后目标分支同步更新；仓库首页取决于 GitHub 默认分支。"
              "每个 PR 的差异属于各自报告，橙/黄色为直接改动，蓝色为潜在关联，未做不算验证通过。"
              "GitHub README 只展示静态图，交互 HTML 在制品内，制品保留 14 天。", README_END]
    return "\n".join(lines)


def sync_readme(root, config, check=False):
    path = root / "README.md"
    original = path.read_bytes().decode("utf-8")
    newline = "\r\n" if "\r\n" in original else "\n"
    block = readme_overview(config).replace("\n", newline)
    if README_START not in original and README_END not in original:
        expected = block + newline * 2 + original
    else:
        if original.count(README_START) != 1 or original.count(README_END) != 1:
            raise ValueError("README architecture markers must be a single pair")
        start, end = original.index(README_START), original.index(README_END)
        if end < start:
            raise ValueError("README architecture markers are reversed")
        expected = original[:start] + block + original[end + len(README_END):]
    if original == expected:
        return False
    if check:
        raise ValueError("README architecture overview is stale: run python scripts/architecture_review.py --readme update and include README.md in the PR")
    path.write_bytes(expected.encode("utf-8"))
    return True


def render_page(name, report):
    template = Path(__file__).with_name(name).read_text(encoding="utf-8")
    payload = json.dumps(report, ensure_ascii=False).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    return template.replace("__REPORT_DATA__", payload)


def evidence_sources(root, ref, config, worktree=False):
    entries = tree(root, ref)
    paths = {valid_path(item["path"]) for item in config.get("evidence", [])}
    if not worktree:
        return read_blobs(root, entries, {p for p in paths if p in entries and entries[p][0] in {"100644", "100755"}})
    return {p: (root / p).read_bytes() for p in paths
            if (root / p).is_file() and not (root / p).is_symlink() and (root / p).resolve().is_relative_to(root)}


def extract_states(data, kind):
    """Read declarations/assignments/references, never import application code.

    This extracts a vocabulary, NOT a control-flow graph or runtime state.
    """
    text = data.decode("utf-8-sig", "replace")
    states = {}
    if kind == "cpp_workflow_enum":
        match = re.search(r"enum\s+class\s+WorkflowState\s*\{([^}]+)\}", text)
        if not match:
            return states
        for token in re.finditer(r"\b[A-Za-z_]\w*\b", re.sub(r"//[^\n]*", "", match[1])):
            value = token.group()
            location = text.find(value, match.start(1))
            states[value] = text.count("\n", 0, location) + 1
        return states
    if kind != "python_job_state":
        raise ValueError(f"Unknown state extractor: {kind}")
    parsed = ast.parse(text)
    def state_attribute(node):
        return isinstance(node, ast.Attribute) and node.attr == "state" and isinstance(node.value, ast.Name) and node.value.id in {"job", "child"}
    def capture(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and re.fullmatch(r"[a-z_]+", node.value):
            states[node.value] = min(states.get(node.value, node.lineno), node.lineno)
        elif isinstance(node, (ast.Set, ast.List, ast.Tuple)):
            for item in node.elts:
                capture(item)
    for node in ast.walk(parsed):
        if isinstance(node, ast.Assign) and any(state_attribute(t) for t in node.targets):
            capture(node.value)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.target.id == "state":
            capture(node.value)
        elif isinstance(node, ast.Compare) and state_attribute(node.left):
            for value in node.comparators:
                capture(value)
    return states


def path_impact(report, paths):
    """File-level evidence only: never claim a changed business condition."""
    paths = set(paths)
    direct = paths & {item["path"] for item in report["changes"]}
    potential = (paths & set(report.get("potential_files", []))) - direct
    return {"kind": "changed" if direct else "potential" if potential else "unchanged",
            "changed": sorted(direct), "potential": sorted(potential)}


def enrich_business(report, config, before, after):
    changed = {item["path"] for item in report["changes"]}
    evidence = []
    for item in config.get("evidence", []):
        data = after.get(item["path"])
        source = data.decode("utf-8-sig", "replace") if data is not None else ""
        offset = source.find(item["needle"])
        evidence.append({**item, "module": module_for(item["path"], config), "found": offset >= 0,
                         "line": source.count("\n", 0, offset) + 1 if offset >= 0 else None,
                         "source_changed": item["path"] in changed,
                         "sha256": digest(data) if data is not None else None})
    by_id = {item["id"]: item for item in evidence}
    journeys = []
    for journey in config.get("journeys", []):
        path = by_id[journey["state_source"]]["path"]
        current = extract_states(after[path], journey["extractor"]) if path in after else {}
        baseline = extract_states(before[path], journey["extractor"]) if path in before else {}
        rows = [{"name": name, "meaning": journey["state_meanings"].get(name, "说明待补充，请复核新状态"),
                 "known": name in journey["state_meanings"], "path": path,
                 "line": current.get(name, baseline.get(name)),
                 "delta": "added" if name not in baseline else "removed" if name not in current else "existing"}
                for name in sorted(current.keys() | baseline.keys())]
        phases = [{**phase, "impact": path_impact(report, [by_id[e]["path"] for e in phase["evidence"] if e in by_id])}
                  for phase in journey["phases"]]
        journeys.append({**journey, "phases": phases, "states": rows,
                         "unrepresented_states": sorted(set(current) - {s for phase in journey["phases"] for s in phase["states"]})})
    flows = []
    for flow in config.get("business_flows", []):
        paths = {by_id[e]["path"] for e in flow["evidence"] if e in by_id}
        if flow.get("impact_scope", "module") == "module":
            paths.update(item["path"] for item in report["changes"] if item.get("module") in flow["modules"])
            paths.update(p for p in report.get("potential_files", []) if module_for(p, config) in flow["modules"])
        flows.append({**flow, "impact": path_impact(report, paths)})
    report["business"] = {"evidence": evidence, "journeys": journeys, "flows": flows,
                          "overview": config.get("product_overview", {}),
                          "source_identity": digest(json.dumps({p: digest(data) for p, data in sorted(after.items())}, sort_keys=True).encode()),
                          "basis": "分层及业务语义由人工维护；状态名称和源码锚点从当前版本提取。锚点存在不证明语义正确，源码变化需复核。",
                          "runtime": "未连接运行中的 Orca 或提供商；不显示实时任务状态，不继承其他版本验收结论。"}


def context_spec(report):
    present = {e["id"] for e in report["business"]["evidence"] if e["found"]}
    relations = [("user", "product", "输入与确认", "generate_confirmation"),
                 ("product", "provider", "确认后外部调用", "provider_authorization"),
                 ("product", "assets", "保存与恢复", "history"),
                 ("product", "output", "原生切片与导出", "native_output")]
    nodes = [("user", "external", "使用者", "文字、图片或已有模型", [60, 260]),
             ("product", "frontend", "Orca 桌面产品", "生成、编辑、切片与预览", [470, 260]),
             ("provider", "cloud", "外部 AI 服务", "经本地 sidecar 与提供商适配", [880, 260]),
             ("assets", "database", "本地项目与资产", "工程、模型与历史记录", [470, 60]),
             ("output", "backend", "打印文件", "G-code；不代表打印完成", [470, 460])]
    return {"schema_version": 1, "diagram_type": "architecture",
            "meta": {"title": "L0 · 产品全貌与边界", "locale": "zh-CN", "quality_profile": "showcase", "legend": {"mode": "hidden"}},
            "components": [{"id": key, "type": kind, "label": label, "sublabel": detail, "pos": pos, "size": [230, 86]} for key, kind, label, detail, pos in nodes],
            "connections": [{"id": f"{a}-{b}", "from": a, "to": b, "label": label,
                             **({"labelDy": 24} if b == "output" else {})} for a,b,label,proof in relations if proof in present],
            "cards": [{"dot": "cyan", "title": "一个桌面产品", "items": ["模型生成与智能切片在同一 Orca 主窗口中", "原生工程、模型、配置与切片由 Orca 管理"]},
                      {"dot": "amber", "title": "确认与边界", "items": ["付费生成在用户确认后发起", "导入模型不等于正式切片或完成打印"]},
                      {"dot": "slate", "title": "图的依据", "items": ["人工维护的产品上下文，附当前源码锚点", f"{sum(not e['found'] for e in report['business']['evidence'])} 个锚点缺失；源码状态不等于运行验证"]}]}


def journey_spec(report, journey):
    found = {e["id"] for e in report["business"]["evidence"] if e["found"]}
    complete = all(set(phase["evidence"]) <= found for phase in journey["phases"])
    states = [{"id": phase["id"], "type": phase["type"], "label": phase["label"], "sublabel": phase["detail"],
               "lane": "main", "col": index, "step": str(index + 1)} for index, phase in enumerate(journey["phases"])]
    lanes = [{"id": "main", "label": "业务阶段（归并状态，不是实时进度）"}]
    current = {state["name"] for state in journey["states"] if state["delta"] != "removed"}
    for group in journey.get("state_groups", []):
        lanes.append({"id": group["id"], "label": group["label"]})
        for index, item in enumerate(group["states"]):
            if item["name"] in current:
                states.append({"id": "state-" + item["name"], "type": item["type"], "label": item["label"],
                               "sublabel": item["name"], "lane": group["id"], "col": index})
    return {"schema_version": 1, "diagram_type": "lifecycle",
            # Trim unused bottom spacing; all authored states remain above the legend.
            # This also enables Archify's native desktop adaptive reading width.
            "meta": {"title": f'L2 · {journey["label"]}主路径' + ("" if complete else "（依据待复核）"), "locale": "zh-CN", "quality_profile": "showcase", "viewBox": [980, 630]},
            "lanes": lanes, "states": states,
            "transitions": [],
            "cards": [{"dot": "amber", "title": "关键条件", "items": [journey["rules"][0]["title"], journey["rules"][1]["title"]]},
                      {"dot": "rose", "title": "异常及等待", "items": [f'{len(journey["states"])} 个源码状态见分层入口的状态表', '未连线的支线状态是清单，触发与恢复条件见分层入口']},
                      {"dot": "slate", "title": "源码依据", "items": ["阶段顺序为维护的业务说明，不是自动推导的完整状态机", "未执行真实生成或切片；验收状态未知"]}]}


def archify_spec(report):
    """A stable overview, using only proven CRG edges; details retain ALL edges.

    Types describe module roles, never change status. Status is explicit node text.
    Five selected dependency pairs make the overview readable and deterministic.
    Missing/new non-backbone edges are counted explicitly, never fabricated.
    """
    positions = {"desktop": [60, 60], "generation": [60, 260], "contracts": [470, 260],
                 "slicing": [880, 260], "adapter": [470, 60], "tests": [470, 460],
                 "core": [880, 60], "engineering": [60, 460], "assets": [880, 460],
                 "unknown": [60, 660]}
    components = []
    for module in report["modules"]:
        key = module["id"]
        if key == "unknown" and not module["changed"] and not module["potential"]:
            continue
        if key not in positions:
            raise ValueError(f"Add and validate an Archify layout for new module: {key}")
        components.append({"id": key, "type": "frontend" if key == "desktop" else "backend",
                           "label": module["label"], "sublabel": f'修改 {module["changed"]} · 潜在关联 {module["potential"]}',
                           "tag": "直接修改" if module["changed"] else "潜在关联" if module["potential"] else "无直接修改",
                           "pos": positions[key], "size": [230, 86]})
    backbone = {("desktop", "generation"), ("generation", "contracts"), ("slicing", "contracts"),
                ("adapter", "contracts"), ("tests", "contracts")}
    connections = []
    for edge in report["relationships"]:
        pair = (edge["source"], edge["target"])
        if pair not in backbone:
            continue
        connection = {"id": "-".join(pair), "from": pair[0], "to": pair[1],
                      "label": {"added": "新增依赖", "removed": "移除依赖", "existing": "静态依赖"}[edge["status"]],
                      "variant": {"added": "emphasis", "removed": "dashed", "existing": "default"}[edge["status"]]}
        if pair in {("desktop", "generation"), ("adapter", "contracts")}:
            # Archify diagnostics placed these vertical labels inside the top node.
            connection["labelDy"] = 24
        connections.append(connection)
    deltas = Counter(edge["status"] for edge in report["relationships"])
    omitted = len(report["relationships"]) - len(connections)
    outside = sum(not c["indexed"] for c in report["changes"])
    return {"schema_version": 1, "diagram_type": "architecture",
            # These are source modules, not independent deployed services.
            "meta": {"title": "Orca · 本次修改影响", "locale": "zh-CN", "quality_profile": "showcase", "legend": {"mode": "hidden"}},
            "components": components, "connections": connections,
            "cards": [
                {"dot": "amber", "title": "修改范围", "items": [
                    f'{len(report["changes"])} 个直接修改文件；{len(report["potential_files"])} 个潜在关联',
                    f'{report["mode"]} · {report["base"][:8]} → {report["head"][:8]}']},
                {"dot": "cyan", "title": "静态依赖总览", "items": [
                    f'展示 {len(connections)} 条主干；其余 {omitted} 条见 details.html',
                    f'全部关系：新增 {deltas["added"]} · 移除 {deltas["removed"]}；依据 code-review-graph']},
                {"dot": "slate", "title": "判断边界", "items": [
                    f'{outside} 个改动文件未解析；完整明细见 details.html / report.json',
                    '仅 AI 范围的静态依赖；动态调用可能遗漏；图不代表测试通过']} ]}


def deliver_archify(root, output, spec, name):
    cli = archify_home(root) / "bin/archify.mjs"
    if not cli.is_file():
        raise RuntimeError("Archify missing: run python scripts/architecture_review.py --setup-archify first")
    package = json.loads((cli.parents[1] / "package.json").read_text(encoding="utf-8"))
    if package["version"] != ARCHIFY_VERSION:
        raise RuntimeError("Archify version differs from the pinned renderer")
    candidate = output / f"{name}.json"
    candidate.write_text(json.dumps(spec, ensure_ascii=False, indent=2), encoding="utf-8")
    environment = {**os.environ, "ARCHIFY_UPDATE_CHECK_DISABLED": "1"}
    for command, suffix in [("validate", "validation"), ("deliver", "receipt")]:
        args = ["node", str(cli), command, spec["diagram_type"], str(candidate)]
        if command == "deliver":
            args.append(str(output / f"{name}.html"))
        result = subprocess.run([*args, "--quality", "showcase", "--json"],
                                capture_output=True, timeout=120, env=environment)
        log = output / f"{name}-{suffix}.json"
        log.write_bytes(result.stdout)
        (output / f"{name}-{suffix}.stderr.log").write_bytes(result.stderr)
        if result.returncode:
            detail = result.stdout.decode("utf-8", "replace")[-1800:]
            raise RuntimeError(f"Archify {command} failed; inspect {log}. Previous HTML is not current evidence.\n{detail}")
        receipt = json.loads(result.stdout)
        if not receipt.get("ok"):
            raise RuntimeError(f"Archify {command} returned no successful receipt")
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--base", default="origin/codex/team/integration")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--worktree", action="store_true")
    parser.add_argument("--setup-archify", action="store_true", help="Download and verify the pinned official renderer, then exit")
    parser.add_argument("--readme", choices=["update", "check"], help="Synchronize/check only the versioned README overview, then exit (stdlib only)")
    parser.add_argument("--output", type=Path, default=Path(".tmp/architecture-review/report"))
    args = parser.parse_args(argv)
    start = time.monotonic()
    root = args.root.resolve()
    if args.readme:
        changed = sync_readme(root, load_map(root), check=args.readme == "check")
        print(json.dumps({"status": "README_UPDATED" if changed else "README_CURRENT"}))
        return 0
    if args.setup_archify:
        setup_archify(root)
        return 0
    output = (root / args.output).resolve()
    # Keep generated content isolated from tracked files and other worktrees.
    if not output.is_relative_to(root / ".tmp"):
        raise ValueError("Output must be inside this checkout's .tmp directory")
    output.mkdir(parents=True, exist_ok=True)
    base_tip = git(root, "rev-parse", "--verify", "--end-of-options", args.base + "^{commit}").decode().strip()
    head = git(root, "rev-parse", "--verify", "--end-of-options", args.head + "^{commit}").decode().strip()
    if args.worktree and head != git(root, "rev-parse", "HEAD").decode().strip():
        raise ValueError("Worktree analysis requires --head HEAD")
    base = git(root, "merge-base", base_tip, head).decode().strip()
    config = load_map(root)
    before_blobs = sources(root, tree(root, base), config)
    after_blobs = sources(root, tree(root, head), config, args.worktree)
    before_business = evidence_sources(root, base, config)
    after_business = evidence_sources(root, head, config, args.worktree)
    changes = change_list(root, base, head, args.worktree)
    cache = root / ".tmp/architecture-review/cache"
    before = analyze(before_blobs, config, cache)
    after = analyze(after_blobs, config, cache)
    if args.worktree and (sources(root, tree(root, head), config, True) != after_blobs or
                         evidence_sources(root, head, config, True) != after_business or change_list(root, base, head, True) != changes):
        raise RuntimeError("Source changed during analysis; report not published. Run again after editing stops.")
    report = make_report(config, before, after, changes, {"base": base, "base_tip": base_tip, "head": head,
                         "mode": "工作区（含未提交修改）" if args.worktree else "PR 提交",
                         "seconds": round(time.monotonic() - start, 2), "map_identity": config["identity"]})
    enrich_business(report, config, before_business, after_business)
    for name, content in [("report.json", json.dumps(report, ensure_ascii=False, indent=2)),
                          ("summary.md", markdown(report)), ("details.html", render_html(report))]:
        (output / name).write_text(content, encoding="utf-8")
    specs = {"l0-context": context_spec(report), "l1-modules": archify_spec(report)}
    specs["l1-modules"]["meta"]["title"] = "L1 · 模块职责与本次改动"
    specs.update({f'l2-{journey["id"]}': journey_spec(report, journey) for journey in report["business"]["journeys"]})
    receipts = {name: deliver_archify(root, output, spec, name) for name, spec in specs.items()}
    (output / "archify-receipts.json").write_text(json.dumps(receipts, ensure_ascii=False, indent=2), encoding="utf-8")
    (output / "index.html").write_text(render_page("architecture_explorer.html", report), encoding="utf-8")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as stream:
            stream.write(markdown(report))
    print(json.dumps({"status": "GENERATED", "changes": len(changes), "seconds": round(time.monotonic() - start, 2),
                      "graph": report["graph"], "html": str(output / "index.html"),
                      "archify": {"version": ARCHIFY_VERSION, "views": {name: receipt["validation"] for name, receipt in receipts.items()}}}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Architecture analysis failed: {error}", file=sys.stderr)
        raise SystemExit(1)

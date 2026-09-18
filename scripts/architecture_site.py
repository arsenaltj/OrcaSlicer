"""Assemble versioned, sandboxed Pages previews from a verified report artifact.

Only static report files are copied; no artifact code is imported or executed.
Repository/PR/run authorization is checked by the trusted publishing workflow.
"""
import argparse
from datetime import datetime, timezone
import hashlib
from html import escape
import json
from pathlib import Path
import re

FILES = ["index.html", "details.html", "report.json", "summary.md", "archify-receipts.json"]
FILES += [view + suffix for view in ["l0-context", "l1-modules", "l2-generation", "l2-slicing"]
          for suffix in [".html", ".json"]]
STYLE = "body{margin:0;background:#eef3f5;color:#173744;font:16px system-ui}main{max-width:1120px;margin:48px auto;padding:24px}a{color:#126777}table{border-collapse:collapse;width:100%;background:white}td,th{padding:15px;text-align:left;border-bottom:1px solid #d9e3e7}small{color:#57717c}header{padding:12px 24px;background:white;border-bottom:1px solid #ccd9dd}iframe{display:block;width:100%;height:calc(100vh - 65px);border:0}"


def page(title, content):
    return '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>' + escape(title) + '</title><style>' + STYLE + '</style>' + content + '</html>\n'


def assemble(report_dir, site, *, pr, run, head, base, repository, title):
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repository):
        raise ValueError("Invalid repository")
    if pr < 1 or run < 1 or not all(re.fullmatch(r"[0-9a-f]{40}", sha) for sha in [head, base]):
        raise ValueError("Invalid PR/run/source identity")
    report_dir, site = Path(report_dir).resolve(), Path(site).resolve()
    payload = {}
    for name in FILES:
        path = report_dir / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
            raise ValueError(f"Missing, linked or oversized report: {name}")
        payload[name] = path.read_bytes()
    report = json.loads(payload["report.json"])
    if report.get("schema") != 1 or report.get("head") != head or report.get("base_tip") != base or report.get("mode") != "PR 提交":
        raise ValueError("Artifact source identity mismatch; refusing publication")
    if sum(map(len, payload.values())) > 32 * 1024 * 1024:
        raise ValueError("Report too large")
    key = f"pr/{pr}/{head}/{run}"
    snapshot = site / key
    for relative in ["history.json", "index.html", "README.md", ".nojekyll", key, f"pr/{pr}/index.html"]:
        path = site / relative
        if path.is_symlink() or not path.resolve().is_relative_to(site):
            raise ValueError("Site path escapes publishing checkout")
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in payload.items()}
    identity_file = snapshot / "identity.json"
    if snapshot.exists():
        if not identity_file.is_file() or json.loads(identity_file.read_text(encoding="utf-8"))["files"] != hashes:
            raise ValueError("Existing version is immutable")
        return key
    history_path = site / "history.json"
    history = json.loads(history_path.read_text(encoding="utf-8")) if history_path.exists() else []
    entry = {"pr": pr, "run": run, "head": head, "base": base, "repository": repository,
             "title": title[:300], "path": key, "created": datetime.now(timezone.utc).isoformat(timespec="seconds")}
    # Treat stored paths as data too: links can only target a known report directory.
    for old in history:
        if not re.fullmatch(r"pr/[1-9][0-9]*/[0-9a-f]{40}/[1-9][0-9]*", old["path"]):
            raise ValueError("Invalid history path")
    view = snapshot / "view"
    view.mkdir(parents=True)
    for name, data in payload.items():
        (view / name).write_bytes(data)
    identity_file.write_text(json.dumps({**entry, "files": hashes}, ensure_ascii=False, indent=2), encoding="utf-8")
    pr_url = f"https://github.com/{repository}/pull/{pr}"
    header = f'<header><a href="../../../../">全部更新记录</a> · <a href="{pr_url}">PR #{pr}</a> · <code>{head[:12]}</code> <small>源码影响报告，不代表业务验收</small></header>'
    # No same-origin/top-navigation permissions: PR JavaScript stays in an opaque frame.
    (snapshot / "index.html").write_text(page(title, header + '<iframe title="交互架构图" sandbox="allow-scripts allow-downloads" referrerpolicy="no-referrer" src="view/index.html"></iframe>'), encoding="utf-8")
    history.append(entry)
    history.sort(key=lambda e: e["run"], reverse=True)
    history_path.write_text(json.dumps(history, ensure_ascii=False, indent=2), encoding="utf-8")
    rows = []
    for item in history:
        rows.append(f'<tr><td><a href="https://github.com/{escape(item["repository"], quote=True)}/pull/{int(item["pr"])}">#{int(item["pr"])}</a></td><td>{escape(item["title"])}</td><td><code>{escape(item["head"][:12])}</code></td><td>{escape(item["created"])}</td><td><a href="{item["path"]}/">打开交互图 →</a></td></tr>')
    landing = '<main><small>ORCA / ARCHITECTURE REVIEW</small><h1>架构更新记录</h1><p>点击直接查看。每条记录绑定 PR、提交和 Actions 运行；旧版本保留，不覆盖。报告只描述源码影响，不代表测试通过。</p><table><thead><tr><th>PR</th><th>修改</th><th>版本</th><th>发布时间 UTC</th><th>交互报告</th></tr></thead><tbody>' + ''.join(rows) + '</tbody></table></main>'
    (site / "index.html").write_text(page("Orca 架构更新记录", landing), encoding="utf-8")
    (site / ".nojekyll").write_text("", encoding="utf-8")
    (site / "README.md").write_text("# Architecture preview artifacts\n\nGenerated static reports only. Source and review remain on the development branches.\n", encoding="utf-8")
    latest = next(item for item in history if item["pr"] == pr)
    relative = latest["path"].removeprefix(f"pr/{pr}/") + "/"
    (site / f"pr/{pr}/index.html").write_text(page(f"PR #{pr}", f'<meta http-equiv="refresh" content="0;url={relative}"><main><a href="{relative}">打开 PR #{pr} 最新已发布报告</a></main>'), encoding="utf-8")
    return key


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--site", required=True, type=Path)
    for name in ["head", "base", "repository", "title"]:
        parser.add_argument("--" + name, required=True)
    for name in ["pr", "run"]:
        parser.add_argument("--" + name, required=True, type=int)
    args = vars(parser.parse_args())
    args["report_dir"] = args.pop("report")
    print(json.dumps({"published_path": assemble(**args)}))

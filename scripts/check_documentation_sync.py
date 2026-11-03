"""Check a PR's documentation declaration against its actual Git changes.

Standard library only. Reads Git and PR JSON as data; never edits documentation,
executes application code, contacts a provider, or posts to GitHub.
"""
from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


UPDATED = "已同步相关文档"
NO_IMPACT = "无需更新维护文档"
BOOKKEEPING = ("docs/plans/", "docs/history/", "docs/audits/", "docs/reports/",
               "docs/coordination/current-development")
# Suggestions route readers to existing instructions, not a second module map.
HINTS = (
    (("src/slic3r/AI/*", "src/slic3r/GUI/AI/*", "src/slic3r/GUI/ModelGeneration*"),
     ("Docs/architecture/PLATFORM_ARCHITECTURE.md", "Docs/architecture/review-map.json")),
    (("src/slic3r/AI/Contracts/*", "src/slic3r/GUI/AI/Model/*", "src/libslic3r/Format/*"),
     ("Docs/domain/printing-color-boundaries.md",)),
    (("src/slic3r/Utils/HonorAccount/*", "src/slic3r/GUI/Redesign/Account*",
      "src/slic3r/GUI/Redesign/Assets*"), ("Docs/architecture/PLATFORM_ARCHITECTURE.md",)),
    (("dev.ps1", "scripts/dev_*", "CMakeLists.txt", "cmake/*", "deps/*", "release/*"),
     ("Docs/coordination/quick-development.md", "release/README.md")),
    ((".github/*", "scripts/check_documentation_sync.py"),
     ("Docs/coordination/documentation-sync.md", "Docs/coordination/team-integration-sop.md")),
)


def git(root: Path, *args: str) -> bytes:
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, timeout=30)
    if result.returncode:
        raise ValueError(f"Git operation failed: {args[0]}; check refs/history and retry")
    return result.stdout


def commit(root: Path, ref: str) -> str:
    if not ref or ref.startswith("-"):
        raise ValueError("A valid base/head commit or ref is required")
    return git(root, "rev-parse", "--verify", ref + "^{commit}").decode().strip()


def path_name(value: str) -> str:
    if (not value or value.startswith(("/", "\\")) or "\\" in value or
            ":" in value or any(ord(c) < 32 for c in value) or
            any(p in {"", ".", ".."} for p in value.split("/"))):
        raise ValueError("Documentation paths must be repository-relative Git paths")
    return value


def changes(root: Path, base: str, head: str, worktree: bool = False) -> tuple[str, str, list[dict]]:
    base, head = commit(root, base), commit(root, head)
    ancestor = git(root, "merge-base", base, head).decode().strip()
    if worktree and head != commit(root, "HEAD"):
        raise ValueError("--worktree requires the current HEAD")
    args = ["diff", "--name-status", "-z", "--find-renames", ancestor]
    if not worktree:
        args.append(head)
    fields = git(root, *args, "--").decode("utf-8").split("\0")
    result = []
    i = 0
    while i < len(fields) and fields[i]:
        status, first = fields[i], path_name(fields[i + 1])
        i += 2
        old = first
        if status.startswith(("R", "C")):
            first = path_name(fields[i]); i += 1
        result.append({"status": status, "old": old, "path": first})
    if worktree:
        for value in git(root, "ls-files", "--others", "--exclude-standard", "-z").decode("utf-8").split("\0"):
            if value:
                result.append({"status": "A", "old": value, "path": path_name(value)})
    return ancestor, head, result


def maintained_doc(path: str) -> bool:
    folded = path.casefold()
    if folded.startswith(BOOKKEEPING) or folded.startswith((".tmp/", ".git/")):
        return False
    return (PurePosixPath(path).suffix.casefold() in {".md", ".rst", ".adoc"} or
            path == "Docs/architecture/review-map.json")


def document(root: Path, revision: str | None, path: str) -> bytes | None:
    if revision is None:
        file = root / path
        if file.is_symlink() or not file.is_file() or not file.resolve().is_relative_to(root.resolve()):
            return None
        return file.read_bytes()
    entry = git(root, "ls-tree", "-z", revision, "--", path)
    if not entry or entry.split(b" ", 1)[0] not in {b"100644", b"100755"}:
        return None
    return git(root, "show", revision + ":" + path)


def normal_text(data: bytes | None) -> str:
    return "" if data is None else data.decode("utf-8-sig").replace("\r\n", "\n").rstrip()


def changed_documents(root: Path, base: str, head: str, entries: list[dict], worktree: bool) -> list[str]:
    result = []
    for entry in entries:
        path = entry["path"]
        if entry["status"].startswith("D") or not maintained_doc(path):
            continue
        after = document(root, None if worktree else head, path)
        before = None if entry["status"].startswith("A") else document(root, base, entry["old"])
        # A deletion, pure rename or line-ending conversion is not a content update.
        if after is not None and normal_text(after) and normal_text(after) != normal_text(before):
            result.append(path)
    return sorted(set(result))


def visible_markdown(text: str) -> str:
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    lines = []; fence = None
    for line in text.splitlines():
        match = re.match(r"^\s*(`{3,}|~{3,})", line)
        if match:
            marker = match[1]
            if fence is None:
                fence = marker
            elif marker[0] == fence[0] and len(marker) >= len(fence):
                fence = None
            continue
        if fence is None:
            lines.append(line)
    return "\n".join(lines)


def declaration(body: str) -> tuple[str, list[str], list[str]]:
    text = visible_markdown(body)
    sections = re.findall(r"^## 文档同步(?:\s*/\s*Documentation)?\s*\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    errors = []
    if len(sections) != 1:
        return "missing", [], ["PR必须包含且仅包含一个“## 文档同步”段落，请使用PR模板。"]
    section = sections[0]
    selected = re.findall(r"^\s*-\s*\[[xX]\]\s*(.*?)\s*$", section, re.M)
    if len(selected) != 1 or selected[0] not in {UPDATED, NO_IMPACT}:
        errors.append("文档同步必须勾选且仅勾选一项：已同步相关文档 / 无需更新维护文档。")
    reason = re.search(r"^说明[：:]\s*(.*)\Z", section, re.M | re.S)
    reason_text = reason[1].strip() if reason else ""
    if reason_text.casefold() in {"", "无", "不适用", "待补充", "n/a", "none", "tbd", "-"}:
        errors.append("文档同步必须填写具体说明；无需更新时说明接口、行为或用法为何不受影响。")
    files = re.search(r"^文档路径[：:]\s*(.*?)(?=^说明[：:]|\Z)", section, re.M | re.S)
    paths = re.findall(r"`([^`\n]+)`", files[1]) if files else []
    try:
        paths = sorted(set(path_name(p) for p in paths))
    except ValueError as error:
        errors.append(str(error)); paths = []
    return selected[0] if len(selected) == 1 else "invalid", paths, errors


def review(root: Path, base: str, head: str, body: str | None = None, *, worktree: bool = False) -> dict:
    base, head, entries = changes(root, base, head, worktree)
    docs = changed_documents(root, base, head, entries, worktree)
    paths = sorted({value for entry in entries for value in (entry["old"], entry["path"])})
    suggested = {"Docs/AI_ENGINEERING.md"} if paths else set()
    for scopes, targets in HINTS:
        if any(fnmatchcase(path, scope) for path in paths for scope in scopes):
            suggested.update(targets)
    errors = []; selected = "not_provided"; declared = []
    if body is not None:
        selected, declared, errors = declaration(body)
        if selected == UPDATED:
            if not declared:
                errors.append("已同步文档时，请在“文档路径”下用反引号列出本PR实际修改的维护文档。")
            for path in declared:
                if path not in docs:
                    errors.append(f"未找到该维护文档的实际内容变更：{path}；进度/历史记录、纯改名或删除不计入。")
        elif selected == NO_IMPACT and (declared or docs):
            errors.append("PR已有维护文档内容变更或列出了文档路径，请选择“已同步相关文档”并说明关联。")
    return {"schema": 1, "mode": "advisory" if body is None else "check", "base": base, "head": head,
            "worktree": worktree, "changed_files": [e["path"] for e in entries],
            "changed_documents": docs, "suggested_documents": sorted(suggested),
            "declaration": selected, "declared_documents": declared, "errors": errors,
            "status": "REVIEW_NEEDED" if body is None else "FAILED" if errors else "PASSED"}


def summary(result: dict) -> str:
    def listing(items):
        payload = "\n".join(json.dumps(p, ensure_ascii=False) for p in items) or "(none)"
        # A fence longer than any untrusted backtick run keeps paths/errors inert.
        longest = max((len(run) for run in re.findall(r"`+", payload)), default=0)
        fence = "`" * max(3, longest + 1)
        return fence + "text\n" + payload + "\n" + fence
    content = (f"## Documentation sync: {result['status']}\n\n"
               f"Changed files: {len(result['changed_files'])}; maintained documents: {len(result['changed_documents'])}.\n\n"
               "Candidate documents to review (not mandatory file edits):\n\n" + listing(result["suggested_documents"]) +
               "\n\nActual documentation content updates:\n\n" + listing(result["changed_documents"]) + "\n")
    if result["errors"]:
        content += "\nErrors:\n\n" + listing(result["errors"]) + "\n"
    return content + "\nThe reviewer still checks semantic relevance; this check does not generate or certify documentation.\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--base", default="HEAD")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--worktree", action="store_true")
    parser.add_argument("--pr-body-file", type=Path)
    parser.add_argument("--event", type=Path, help="GitHub pull_request event JSON")
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args(argv)
    try:
        body = args.pr_body_file.read_text(encoding="utf-8-sig") if args.pr_body_file else None
        if args.event:
            if args.worktree or args.pr_body_file:
                raise ValueError("--event cannot be mixed with --worktree or --pr-body-file")
            pr = json.loads(args.event.read_text(encoding="utf-8"))["pull_request"]
            args.base, args.head, body = pr["base"]["sha"], pr["head"]["sha"], pr.get("body") or ""
        result = review(args.root.resolve(), args.base, args.head, body, worktree=args.worktree)
        if args.summary:
            with args.summary.open("a", encoding="utf-8") as stream:
                stream.write(summary(result))
        print(json.dumps(result, ensure_ascii=True, indent=2))
        return int(bool(result["errors"]))
    except (ValueError, KeyError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Documentation check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

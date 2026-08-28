"""Collect a frozen, license-audited Wikimedia Commons benchmark source set."""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request
from urllib.error import HTTPError
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping

from PIL import Image, ImageDraw, ImageOps


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
COMMONS_API = "https://commons.wikimedia.org/w/api.php"
USER_AGENT = (
    "OrcaSlicer-Image2-Quality-Benchmark/1.0 "
    "(internal quality evaluation; contact: https://github.com/SoftFever/OrcaSlicer)"
)
SOURCE_MANIFEST = "source-manifest.json"
BENCHMARK_MANIFEST = "benchmark-manifest.json"
MINIMUM_EDGE = 512
MAXIMUM_SOURCE_PIXELS = 100_000_000
THUMBNAIL_WIDTH = 1280
ALLOWED_MIME_TYPES = {"image/jpeg", "image/png"}
EXCLUDED_TITLE_TERMS = {
    "advertising", "chart", "coat of arms", "diagram", "drawing", "flag", "floor plan", "icon", "logo",
    "map", "montage", "newspaper", "painting", "poster", "scan", "signature", "stamp", "text",
}
HUMAN_CONTEXT_TERMS = {
    "boy", "boys", "child", "children", "cosplay", "girl", "girls", "holding", "man", "people", "person",
    "wearing", "woman",
}
QUERY_STOP_WORDS = {
    "a", "an", "and", "body", "close", "full", "front", "object", "of", "on", "photo", "photograph",
    "portrait", "side", "the", "three", "view", "with",
}
_LAST_API_REQUEST_AT = 0.0


class CommonsSourceError(RuntimeError):
    pass


@dataclass(frozen=True)
class SourceCase:
    case_id: str
    label: str
    category: str
    community_use: str
    instruction: str
    challenges: tuple[str, ...]
    preserve: tuple[str, ...]
    queries: tuple[str, ...]
    commons_categories: tuple[str, ...]
    preferred_title: str = ""


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CommonsSourceError(f"Cannot read JSON: {path}") from exc
    if not isinstance(value, dict):
        raise CommonsSourceError(f"JSON root must be an object: {path}")
    return value


def _atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    # Windows file indexers and antivirus scanners may briefly hold the previous
    # manifest open. Keep the write atomic, but tolerate that short-lived lock.
    for attempt in range(7):
        try:
            os.replace(temporary, path)
            return
        except PermissionError:
            if attempt == 6:
                raise
            time.sleep(0.05 * (2**attempt))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _string_list(value: object, field: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not value:
        raise CommonsSourceError(f"{field} must be a non-empty list")
    result: list[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip():
            raise CommonsSourceError(f"{field} contains an invalid string")
        cleaned = item.strip()
        if cleaned not in result:
            result.append(cleaned)
    return tuple(result)


def flatten_plan(plan: Mapping[str, Any]) -> list[SourceCase]:
    groups = plan.get("groups")
    if not isinstance(groups, list) or not groups:
        raise CommonsSourceError("Source plan requires groups")
    cases: list[SourceCase] = []
    seen: set[str] = set()
    category_map = plan.get("commons_categories", {})
    if not isinstance(category_map, dict):
        raise CommonsSourceError("commons_categories must be an object")
    preferred_titles = plan.get("preferred_titles", {})
    if not isinstance(preferred_titles, dict):
        raise CommonsSourceError("preferred_titles must be an object")
    for group in groups:
        if not isinstance(group, dict):
            raise CommonsSourceError("Each source-plan group must be an object")
        category = group.get("category")
        community_use = group.get("community_use")
        instruction = group.get("instruction")
        if not all(isinstance(value, str) and value.strip() for value in (category, community_use, instruction)):
            raise CommonsSourceError("Each group requires category, community_use and instruction")
        challenges = _string_list(group.get("challenges"), f"{category}.challenges")
        preserve = _string_list(group.get("preserve"), f"{category}.preserve")
        raw_cases = group.get("cases")
        if not isinstance(raw_cases, list) or not raw_cases:
            raise CommonsSourceError(f"Group {category} requires cases")
        for raw_case in raw_cases:
            if not isinstance(raw_case, dict):
                raise CommonsSourceError(f"Group {category} has an invalid case")
            case_id = raw_case.get("id")
            label = raw_case.get("label")
            if not isinstance(case_id, str) or not re.fullmatch(r"[a-z0-9_]+", case_id) or case_id in seen:
                raise CommonsSourceError(f"Invalid or duplicate case id: {case_id}")
            if not isinstance(label, str) or not label.strip():
                raise CommonsSourceError(f"Case {case_id} requires a label")
            case_instruction = raw_case.get("instruction", instruction)
            if not isinstance(case_instruction, str) or not case_instruction.strip():
                raise CommonsSourceError(f"Case {case_id} has an invalid instruction override")
            case_challenges = (
                _string_list(raw_case.get("challenges"), f"{case_id}.challenges")
                if "challenges" in raw_case else challenges
            )
            case_preserve = (
                _string_list(raw_case.get("preserve"), f"{case_id}.preserve")
                if "preserve" in raw_case else preserve
            )
            seen.add(case_id)
            cases.append(SourceCase(
                case_id=case_id,
                label=label.strip(),
                category=category.strip(),
                community_use=community_use.strip(),
                instruction=case_instruction.strip(),
                challenges=case_challenges,
                preserve=case_preserve,
                queries=_string_list(raw_case.get("queries"), f"{case_id}.queries"),
                commons_categories=(
                    _string_list(category_map.get(case_id), f"commons_categories.{case_id}")
                    if case_id in category_map else ()
                ),
                preferred_title=_plain_text(preferred_titles.get(case_id), 240),
            ))
    return cases


def _plain_text(value: object, limit: int = 500) -> str:
    if not isinstance(value, str):
        return ""
    without_tags = re.sub(r"<[^>]*>", " ", value)
    return " ".join(html.unescape(without_tags).split())[:limit]


def _upgrade_https_url(value: object) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    parsed = urllib.parse.urlsplit(text)
    if parsed.scheme.lower() == "http" and parsed.netloc:
        return urllib.parse.urlunsplit(("https", parsed.netloc, parsed.path, parsed.query, parsed.fragment))
    return text


def _metadata_value(info: Mapping[str, Any], key: str) -> str:
    metadata = info.get("extmetadata")
    if not isinstance(metadata, dict):
        return ""
    item = metadata.get(key)
    if not isinstance(item, dict):
        return ""
    return _plain_text(item.get("value"))


def allowed_license(name: str, url: str) -> bool:
    normalized = " ".join(name.lower().replace("-", " ").split())
    if any(term in normalized for term in ("non commercial", "no derivatives", "fair use", "copyrighted")):
        return False
    if re.search(r"(?:^| )cc (?:by )?(?:nc|nd)(?: |$)", normalized):
        return False
    if normalized in {"public domain", "pd", "cc0", "cc zero"}:
        return True
    if normalized.startswith("cc by"):
        return bool(url and ("creativecommons.org" in url.lower()))
    return False


def _query_tokens(query: str) -> set[str]:
    return {
        token for token in re.findall(r"[a-z0-9]+", query.lower())
        if len(token) > 2 and token not in QUERY_STOP_WORDS
    }


def _candidate_record(
    page: Mapping[str, Any],
    query: str,
    rank: int,
    *,
    minimum_matches: int | None = None,
    object_only: bool = False,
    rank_weight: int = 2,
) -> dict[str, Any] | None:
    imageinfo = page.get("imageinfo")
    if not isinstance(imageinfo, list) or not imageinfo or not isinstance(imageinfo[0], dict):
        return None
    info = imageinfo[0]
    title = _plain_text(page.get("title"), 240)
    mime = str(info.get("mime", "")).lower()
    mediatype = str(info.get("mediatype", "")).upper()
    width = info.get("width")
    height = info.get("height")
    if mime not in ALLOWED_MIME_TYPES or mediatype != "BITMAP":
        return None
    if isinstance(width, bool) or not isinstance(width, int) or isinstance(height, bool) or not isinstance(height, int):
        return None
    if min(width, height) < MINIMUM_EDGE or width * height > MAXIMUM_SOURCE_PIXELS:
        return None
    ratio = width / height
    if not 0.4 <= ratio <= 2.5:
        return None
    lowered_title = title.lower()
    if any(term in lowered_title for term in EXCLUDED_TITLE_TERMS):
        return None
    if object_only and (_query_tokens(title) & HUMAN_CONTEXT_TERMS):
        return None
    license_name = _metadata_value(info, "LicenseShortName")
    license_url = _upgrade_https_url(_metadata_value(info, "LicenseUrl"))
    if not allowed_license(license_name, license_url):
        return None
    url = str(info.get("thumburl") or info.get("url") or "")
    if not url.startswith("https://"):
        return None
    tokens = _query_tokens(query)
    matches = sum(token in lowered_title for token in tokens)
    required_matches = min(2, len(tokens)) if minimum_matches is None else minimum_matches
    if matches < required_matches:
        return None
    if any(f"without {token}" in lowered_title for token in tokens):
        return None
    license_bonus = 8 if license_name.lower() in {"public domain", "pd", "cc0", "cc zero"} else 4
    score = (
        max(0, 30 - rank) * rank_weight
        + matches * 12
        + license_bonus
        + min(8, int((width * height) / 2_000_000))
    )
    return {
        "query": query,
        "search_rank": rank,
        "score": score,
        "page_id": page.get("pageid"),
        "commons_title": title,
        "source_page": _upgrade_https_url(info.get("descriptionurl") or info.get("descriptionshorturl")),
        "source_url": str(info.get("url") or ""),
        "download_url": url,
        "commons_sha1": str(info.get("sha1") or ""),
        "original_width": width,
        "original_height": height,
        "mime": mime,
        "license": license_name,
        "license_url": license_url,
        "artist": _metadata_value(info, "Artist") or "Unknown",
        "credit": _metadata_value(info, "Credit"),
        "attribution_required": _metadata_value(info, "AttributionRequired"),
        "usage_terms": _metadata_value(info, "UsageTerms"),
        "restrictions": _metadata_value(info, "Restrictions"),
        "image_description": _metadata_value(info, "ImageDescription"),
    }


def _request_bytes(url: str, timeout: float = 30.0, attempts: int = 3) -> bytes:
    error: Exception | None = None
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read()
        except Exception as exc:  # transient public-network errors are safe to retry
            error = exc
            if attempt + 1 < attempts:
                throttled = isinstance(exc, HTTPError) and exc.code in {403, 429}
                time.sleep((10.0 if throttled else 1.5) * (attempt + 1))
    raise CommonsSourceError(f"GET failed after {attempts} attempts: {url}") from error


def _fetch_json(url: str) -> dict[str, Any]:
    try:
        value = json.loads(_request_bytes(url).decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise CommonsSourceError(f"Invalid API response: {url}") from exc
    if not isinstance(value, dict):
        raise CommonsSourceError("Commons API response must be an object")
    return value


def search_commons(
    query: str,
    *,
    object_only: bool = False,
    minimum_matches: int | None = None,
    fetch_json: Callable[[str], Mapping[str, Any]] = _fetch_json,
) -> list[dict[str, Any]]:
    global _LAST_API_REQUEST_AT
    if fetch_json is _fetch_json:
        wait = 1.1 - (time.monotonic() - _LAST_API_REQUEST_AT)
        if wait > 0:
            time.sleep(wait)
    parameters = {
        "action": "query",
        "format": "json",
        "formatversion": "2",
        "generator": "search",
        "gsrsearch": query,
        "gsrnamespace": "6",
        "gsrlimit": "12",
        "gsrsort": "relevance",
        "prop": "imageinfo",
        "iiprop": "url|size|mime|sha1|mediatype|extmetadata",
        "iiurlwidth": str(THUMBNAIL_WIDTH),
        "iiextmetadatalanguage": "en",
        "iiextmetadatafilter": (
            "LicenseShortName|LicenseUrl|Artist|Credit|AttributionRequired|Restrictions|ImageDescription|UsageTerms"
        ),
    }
    payload = fetch_json(COMMONS_API + "?" + urllib.parse.urlencode(parameters))
    if fetch_json is _fetch_json:
        _LAST_API_REQUEST_AT = time.monotonic()
    query_value = payload.get("query")
    pages = query_value.get("pages", []) if isinstance(query_value, dict) else []
    if not isinstance(pages, list):
        return []
    records = [
        record for rank, page in enumerate(pages)
        if isinstance(page, dict) and (
            record := _candidate_record(
                page,
                query,
                rank,
                minimum_matches=minimum_matches,
                object_only=object_only,
            )
        ) is not None
    ]
    return sorted(records, key=lambda item: (-int(item["score"]), int(item["search_rank"]), item["commons_title"]))


def search_commons_category(
    commons_category: str,
    query: str,
    *,
    object_only: bool,
    fetch_json: Callable[[str], Mapping[str, Any]] = _fetch_json,
) -> list[dict[str, Any]]:
    global _LAST_API_REQUEST_AT
    if fetch_json is _fetch_json:
        wait = 1.1 - (time.monotonic() - _LAST_API_REQUEST_AT)
        if wait > 0:
            time.sleep(wait)
    parameters = {
        "action": "query",
        "format": "json",
        "formatversion": "2",
        "generator": "categorymembers",
        "gcmtitle": "Category:" + commons_category.removeprefix("Category:"),
        "gcmtype": "file",
        "gcmlimit": "50",
        "prop": "imageinfo",
        "iiprop": "url|size|mime|sha1|mediatype|extmetadata",
        "iiurlwidth": str(THUMBNAIL_WIDTH),
        "iiextmetadatalanguage": "en",
        "iiextmetadatafilter": (
            "LicenseShortName|LicenseUrl|Artist|Credit|AttributionRequired|Restrictions|ImageDescription|UsageTerms"
        ),
    }
    payload = fetch_json(COMMONS_API + "?" + urllib.parse.urlencode(parameters))
    if fetch_json is _fetch_json:
        _LAST_API_REQUEST_AT = time.monotonic()
    query_value = payload.get("query")
    pages = query_value.get("pages", []) if isinstance(query_value, dict) else []
    if not isinstance(pages, list):
        return []
    records = [
        record for rank, page in enumerate(pages)
        if isinstance(page, dict) and (
            record := _candidate_record(
                page,
                query,
                rank,
                minimum_matches=0,
                object_only=object_only,
                rank_weight=0,
            )
        ) is not None
    ]
    return sorted(records, key=lambda item: (-int(item["score"]), int(item["search_rank"]), item["commons_title"]))


def lookup_commons_file(
    title: str,
    query: str,
    fetch_json: Callable[[str], Mapping[str, Any]] = _fetch_json,
) -> dict[str, Any] | None:
    global _LAST_API_REQUEST_AT
    normalized_title = title if title.startswith("File:") else "File:" + title
    if fetch_json is _fetch_json:
        wait = 1.1 - (time.monotonic() - _LAST_API_REQUEST_AT)
        if wait > 0:
            time.sleep(wait)
    parameters = {
        "action": "query",
        "format": "json",
        "formatversion": "2",
        "titles": normalized_title,
        "prop": "imageinfo",
        "iiprop": "url|size|mime|sha1|mediatype|extmetadata",
        "iiurlwidth": str(THUMBNAIL_WIDTH),
        "iiextmetadatalanguage": "en",
        "iiextmetadatafilter": (
            "LicenseShortName|LicenseUrl|Artist|Credit|AttributionRequired|Restrictions|ImageDescription|UsageTerms"
        ),
    }
    payload = fetch_json(COMMONS_API + "?" + urllib.parse.urlencode(parameters))
    if fetch_json is _fetch_json:
        _LAST_API_REQUEST_AT = time.monotonic()
    query_value = payload.get("query")
    pages = query_value.get("pages", []) if isinstance(query_value, dict) else []
    if not isinstance(pages, list) or not pages or not isinstance(pages[0], dict):
        return None
    return _candidate_record(pages[0], query, 0, minimum_matches=0)


def _download_image(record: Mapping[str, Any], destination: Path) -> dict[str, Any]:
    temporary = destination.with_name(destination.name + ".part")
    temporary.parent.mkdir(parents=True, exist_ok=True)
    temporary.write_bytes(_request_bytes(str(record["download_url"])))
    try:
        with Image.open(temporary) as source:
            source.load()
            image = ImageOps.exif_transpose(source).convert("RGB")
            if min(image.size) < MINIMUM_EDGE:
                raise CommonsSourceError("Downloaded thumbnail is below the minimum edge")
            if max(image.size) > THUMBNAIL_WIDTH:
                image.thumbnail((THUMBNAIL_WIDTH, THUMBNAIL_WIDTH), Image.Resampling.LANCZOS)
            image.save(destination, format="JPEG", quality=95, optimize=True)
        with Image.open(destination) as verification:
            verification.verify()
        with Image.open(destination) as normalized:
            width, height = normalized.size
    finally:
        temporary.unlink(missing_ok=True)
    return {
        "source_sha256": _sha256(destination),
        "downloaded_width": width,
        "downloaded_height": height,
        "downloaded_format": "JPEG",
    }


def build_benchmark_manifest(records: Iterable[Mapping[str, Any]]) -> dict[str, Any]:
    cases = []
    for record in records:
        cases.append({
            "id": record["id"],
            "source": record["source"],
            "instruction": record["instruction"],
            "label": record["label"],
            "category": record["category"],
            "challenges": record["challenges"],
            "preserve": record["preserve"],
            "community_use": record["community_use"],
            "source_page": _upgrade_https_url(record["source_page"]),
            "license": record["license"],
            "license_url": _upgrade_https_url(record["license_url"]),
            "attribution": record["artist"],
        })
    return {
        "schema_version": 1,
        "description": "100 source images x (1 sculpture + 2 realistic palettes + 2 cartoon palettes) = 500 outputs.",
        "palettes": {
            "warm": ["#C95B43", "#253B5E", "#F2E5C4", "#D6A72C"],
            "cool": ["#3B82C4", "#293241", "#E8F0F2", "#E76F51"],
        },
        "style_runs": {
            "sculpture": [{"palette": None, "repetitions": 1}],
            "realistic": [
                {"palette": "warm", "repetitions": 1},
                {"palette": "cool", "repetitions": 1},
            ],
            "cartoon": [
                {"palette": "warm", "repetitions": 1},
                {"palette": "cool", "repetitions": 1},
            ],
        },
        "cases": cases,
    }


def _review_tile(path: Path, case_id: str, category: str, size: int = 260) -> Image.Image:
    tile = Image.new("RGB", (size, size + 42), "white")
    with Image.open(path) as source:
        image = ImageOps.exif_transpose(source).convert("RGB")
        image.thumbnail((size - 12, size - 12), Image.Resampling.LANCZOS)
        tile.paste(image, ((size - image.width) // 2, (size - image.height) // 2))
    draw = ImageDraw.Draw(tile)
    draw.text((6, size + 4), case_id[:34], fill="black")
    draw.text((6, size + 20), category[:34], fill=(80, 80, 80))
    return tile


def create_source_review_sheets(output_root: Path, records: Iterable[Mapping[str, Any]]) -> int:
    rows = list(records)
    destination_root = output_root / "source-review-sheets"
    destination_root.mkdir(parents=True, exist_ok=True)
    page_count = 0
    for start in range(0, len(rows), 20):
        page = rows[start:start + 20]
        tiles = [
            _review_tile(REPOSITORY_ROOT / str(record["source"]), str(record["id"]), str(record["category"]))
            for record in page
        ]
        canvas = Image.new("RGB", (5 * 260, 4 * 302), (238, 240, 243))
        for index, tile in enumerate(tiles):
            canvas.paste(tile, ((index % 5) * 260, (index // 5) * 302))
        page_count += 1
        canvas.save(destination_root / f"source-review-{page_count:02d}.jpg", quality=92, optimize=True)
    return page_count


def create_candidate_review(
    plan_path: Path,
    output_root: Path,
    case_ids: Iterable[str],
    candidates_per_case: int = 6,
) -> dict[str, Any]:
    cases = flatten_plan(_read_json(plan_path))
    wanted = set(case_ids)
    selected = [case for case in cases if not wanted or case.case_id in wanted]
    missing = sorted(wanted - {case.case_id for case in selected})
    if missing:
        raise CommonsSourceError("Unknown review case IDs: " + ", ".join(missing))
    source_manifest_path = output_root / SOURCE_MANIFEST
    source_manifest = _read_json(source_manifest_path) if source_manifest_path.is_file() else {}
    current_records = {
        str(record.get("id")): record for record in source_manifest.get("records", [])
        if isinstance(record, dict) and isinstance(record.get("id"), str)
    }
    review_root = output_root / "source-candidate-review"
    rows: list[list[tuple[Path | None, str]]] = []
    catalog: list[dict[str, Any]] = []
    for index, case in enumerate(selected, start=1):
        candidates: dict[str, dict[str, Any]] = {}
        for commons_category in case.commons_categories:
            for candidate in search_commons_category(
                commons_category,
                case.queries[0],
                object_only=not case.category.startswith("portrait"),
            ):
                identity = str(candidate.get("commons_sha1") or candidate.get("source_url"))
                if identity:
                    candidates.setdefault(identity, candidate)
        if len(candidates) < candidates_per_case:
            for query in case.queries:
                for candidate in search_commons(
                    query,
                    object_only=not case.category.startswith("portrait"),
                    minimum_matches=0,
                ):
                    identity = str(candidate.get("commons_sha1") or candidate.get("source_url"))
                    if identity:
                        candidates.setdefault(identity, candidate)
                if len(candidates) >= candidates_per_case:
                    break
        ranked = sorted(candidates.values(), key=lambda item: (-int(item["score"]), item["commons_title"]))
        current = current_records.get(case.case_id)
        current_path = REPOSITORY_ROOT / str(current.get("source")) if current else None
        row: list[tuple[Path | None, str]] = [(current_path, f"{case.case_id} CURRENT")]
        case_catalog = {"id": case.case_id, "label": case.label, "candidates": []}
        for candidate_index, candidate in enumerate(ranked[:candidates_per_case], start=1):
            destination = review_root / case.case_id / f"{candidate_index:02d}.jpg"
            try:
                local = _download_image(candidate, destination)
            except Exception:
                continue
            row.append((destination, f"{candidate_index}: {candidate['commons_title'].removeprefix('File:')[:28]}"))
            case_catalog["candidates"].append({"index": candidate_index, **candidate, **local})
        while len(row) < candidates_per_case + 1:
            row.append((None, "NO CANDIDATE"))
        rows.append(row)
        catalog.append(case_catalog)
        print(json.dumps({
            "index": index,
            "id": case.case_id,
            "candidate_count": len(case_catalog["candidates"]),
        }), flush=True)
    tile_size = 220
    tile_height = tile_size + 42
    pages = 0
    sheet_root = review_root / "sheets"
    sheet_root.mkdir(parents=True, exist_ok=True)
    for start in range(0, len(rows), 3):
        page_rows = rows[start:start + 3]
        canvas = Image.new("RGB", ((candidates_per_case + 1) * tile_size, len(page_rows) * tile_height), (238, 240, 243))
        for row_index, row in enumerate(page_rows):
            for column, (path, label) in enumerate(row):
                tile = _review_tile(path, label, "candidate", tile_size) if path and path.is_file() else Image.new(
                    "RGB", (tile_size, tile_height), (220, 222, 226)
                )
                canvas.paste(tile, (column * tile_size, row_index * tile_height))
        pages += 1
        canvas.save(sheet_root / f"candidate-review-{pages:02d}.jpg", quality=92, optimize=True)
    _atomic_json(review_root / "candidate-review.json", {"cases": catalog})
    return {"case_count": len(selected), "pages": pages, "catalog": str(review_root / "candidate-review.json")}


def collect(plan_path: Path, output_root: Path, target_count: int) -> dict[str, Any]:
    output_root = output_root.resolve()
    try:
        output_root.relative_to(REPOSITORY_ROOT.resolve())
    except ValueError:
        raise CommonsSourceError("Output root must stay inside the repository") from None
    cases = flatten_plan(_read_json(plan_path))
    if len(cases) != target_count:
        raise CommonsSourceError(f"Source plan expands to {len(cases)} cases; expected {target_count}")
    plan_sha256 = _sha256(plan_path)
    manifest_path = output_root / SOURCE_MANIFEST
    existing = _read_json(manifest_path) if manifest_path.is_file() else {}
    if existing and existing.get("plan_sha256") != plan_sha256:
        raise CommonsSourceError("Existing source set belongs to a different frozen plan")
    records = existing.get("records", []) if isinstance(existing.get("records"), list) else []
    for record in records:
        if isinstance(record, dict):
            record["source_page"] = _upgrade_https_url(record.get("source_page"))
            record["license_url"] = _upgrade_https_url(record.get("license_url"))
    records_by_id = {
        str(record.get("id")): record for record in records
        if isinstance(record, dict) and isinstance(record.get("id"), str)
    }
    used_commons_sha1 = {
        str(record.get("commons_sha1")) for record in records_by_id.values() if record.get("commons_sha1")
    }
    used_source_sha256 = {
        str(record.get("source_sha256")) for record in records_by_id.values() if record.get("source_sha256")
    }
    failures: list[dict[str, str]] = []
    for index, case in enumerate(cases, start=1):
        existing_record = records_by_id.get(case.case_id)
        if existing_record:
            source = REPOSITORY_ROOT / str(existing_record.get("source", ""))
            if source.is_file() and _sha256(source) == existing_record.get("source_sha256"):
                print(json.dumps({"index": index, "id": case.case_id, "status": "reused"}), flush=True)
                continue
            raise CommonsSourceError(f"Frozen source is missing or changed: {case.case_id}")
        candidates: dict[str, dict[str, Any]] = {}
        if case.preferred_title:
            preferred = lookup_commons_file(case.preferred_title, case.queries[0])
            if preferred is not None:
                candidates[str(preferred.get("commons_sha1") or preferred.get("source_url"))] = preferred
        for commons_category in case.commons_categories:
            for candidate in search_commons_category(
                commons_category,
                case.queries[0],
                object_only=not case.category.startswith("portrait"),
            ):
                identity = str(candidate.get("commons_sha1") or candidate.get("source_url"))
                if identity and identity not in candidates:
                    candidates[identity] = candidate
            if candidates and case.preferred_title:
                break
        for query in case.queries:
            if candidates:
                break
            for candidate in search_commons(query, object_only=not case.category.startswith("portrait")):
                identity = str(candidate.get("commons_sha1") or candidate.get("source_url"))
                if identity and identity not in candidates:
                    candidates[identity] = candidate
            if candidates:
                break
        chosen: dict[str, Any] | None = None
        error = "No acceptable licensed bitmap candidate"
        for candidate in sorted(
            candidates.values(),
            key=lambda item: (
                0 if case.preferred_title and item["commons_title"] == case.preferred_title else 1,
                -int(item["score"]),
            ),
        ):
            commons_sha1 = str(candidate.get("commons_sha1", ""))
            if commons_sha1 and commons_sha1 in used_commons_sha1:
                continue
            destination = output_root / "sources" / f"{case.case_id}.jpg"
            try:
                local = _download_image(candidate, destination)
            except Exception as exc:
                error = f"{type(exc).__name__}: {exc}"[:300]
                continue
            if local["source_sha256"] in used_source_sha256:
                destination.unlink(missing_ok=True)
                continue
            relative_source = destination.relative_to(REPOSITORY_ROOT).as_posix()
            chosen = {
                "id": case.case_id,
                "label": case.label,
                "category": case.category,
                "community_use": case.community_use,
                "instruction": case.instruction,
                "challenges": list(case.challenges),
                "preserve": list(case.preserve),
                "queries": list(case.queries),
                "commons_categories": list(case.commons_categories),
                "source": relative_source,
                **candidate,
                **local,
                "modifications": "Scaled to at most 1280 px, EXIF orientation applied, converted to RGB JPEG.",
            }
            records_by_id[case.case_id] = chosen
            if commons_sha1:
                used_commons_sha1.add(commons_sha1)
            used_source_sha256.add(str(local["source_sha256"]))
            ordered_records = [records_by_id[item.case_id] for item in cases if item.case_id in records_by_id]
            _atomic_json(manifest_path, {
                "schema_version": 1,
                "plan": plan_path.relative_to(REPOSITORY_ROOT).as_posix(),
                "plan_sha256": plan_sha256,
                "record_count": len(ordered_records),
                "target_count": target_count,
                "records": ordered_records,
            })
            print(json.dumps({
                "index": index,
                "id": case.case_id,
                "status": "downloaded",
                "license": chosen["license"],
                "title": chosen["commons_title"],
            }, ensure_ascii=False), flush=True)
            break
        if chosen is None:
            failures.append({"id": case.case_id, "error": error})
            print(json.dumps({"index": index, "id": case.case_id, "status": "failed", "error": error}), flush=True)
    ordered_records = [records_by_id[item.case_id] for item in cases if item.case_id in records_by_id]
    _atomic_json(manifest_path, {
        "schema_version": 1,
        "plan": plan_path.relative_to(REPOSITORY_ROOT).as_posix(),
        "plan_sha256": plan_sha256,
        "record_count": len(ordered_records),
        "target_count": target_count,
        "failures": failures,
        "records": ordered_records,
    })
    if len(ordered_records) != target_count:
        raise CommonsSourceError(f"Collected {len(ordered_records)}/{target_count} sources; inspect {manifest_path}")
    benchmark_manifest = build_benchmark_manifest(ordered_records)
    _atomic_json(output_root / BENCHMARK_MANIFEST, benchmark_manifest)
    pages = create_source_review_sheets(output_root, ordered_records)
    return {
        "source_count": len(ordered_records),
        "candidate_count": len(ordered_records) * 5,
        "source_manifest": str(manifest_path),
        "benchmark_manifest": str(output_root / BENCHMARK_MANIFEST),
        "source_review_pages": pages,
    }


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="Collect a 100-image, license-audited Commons benchmark source set.")
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target-count", type=int, default=100)
    parser.add_argument("--review-candidates", action="store_true")
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    if args.target_count < 1:
        parser.error("--target-count must be positive")
    try:
        if args.review_candidates:
            result = create_candidate_review(args.plan.resolve(), args.output.resolve(), args.case)
        else:
            result = collect(args.plan.resolve(), args.output, args.target_count)
    except CommonsSourceError as exc:
        parser.error(str(exc))
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Debug original-image preprocessing without starting Orca or a 3D task.

The default records a prompt and local diagnostics only. --generate performs
one image edit; --semantic-review explicitly requests one vision review.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path

try:
    from .image_preprocessing_policy import CATEGORIES
    from .model_input_image_quality import assess_model_input_image
    from .nonportrait_reference import prepare_nonportrait_views, review_nonportrait_reference
    from .openai_preprocessor import (
        CUSTOM_STYLE_ID, STYLE_PROFILES, build_geometry_reference_prompt_layers, complete_vision_once,
        image_preprocessing_policy, preprocess_image,
    )
except ImportError:
    from image_preprocessing_policy import CATEGORIES
    from model_input_image_quality import assess_model_input_image
    from nonportrait_reference import prepare_nonportrait_views, review_nonportrait_reference
    from openai_preprocessor import (
        CUSTOM_STYLE_ID, STYLE_PROFILES, build_geometry_reference_prompt_layers, complete_vision_once,
        image_preprocessing_policy, preprocess_image,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--instruction", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--style", choices=sorted({*STYLE_PROFILES, CUSTOM_STYLE_ID}), default="realistic")
    parser.add_argument("--custom-style", default="", help="Description required with --style custom")
    parser.add_argument("--category", choices=CATEGORIES, default="")
    parser.add_argument("--subject-text", choices=("auto", "preserve", "remove"), default="auto")
    parser.add_argument("--matte-proxy", action="store_true")
    parser.add_argument("--print-settings", default="{}", help="JSON using the existing print settings contract")
    parser.add_argument("--view", action="append", default=[], metavar="NAME=PATH", help="Supplied real front/left/back/right references")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--prepared", type=Path, help="Compare an existing processed image")
    source.add_argument("--generate", action="store_true", help="Perform exactly one configured image edit")
    parser.add_argument("--semantic-review", action="store_true", help="Request one configured vision review, without retries")
    args = parser.parse_args(argv)
    if args.semantic_review and not (args.generate or args.prepared):
        parser.error("--semantic-review requires --generate or --prepared")
    try:
        settings = json.loads(args.print_settings)
        options = {"filename": args.input.name, "category_hint": args.category,
                   "subject_text": args.subject_text, "material_proxy": args.matte_proxy}
        policy = image_preprocessing_policy(args.instruction, args.style, custom_style=args.custom_style, print_settings=settings,
                                            preprocessing_options=options)
        if args.matte_proxy and not policy["specialized"]:
            parser.error("--matte-proxy requires an identified non-portrait subject")
        if args.semantic_review and not policy["specialized"]:
            parser.error("This semantic reviewer requires an identified non-portrait subject")
        layers = build_geometry_reference_prompt_layers(args.instruction, args.style, args.custom_style, print_settings=settings,
                                                         preprocessing_options=options)
        prompt = "".join(layer["text"] for layer in layers)
        original_quality = assess_model_input_image(args.input)
        views = {}
        for value in args.view:
            name, separator, path = value.partition("=")
            if not separator or name in views:
                parser.error("Each --view must supply a distinct NAME=PATH")
            views[name] = Path(path)
        if args.prepared:
            assess_model_input_image(args.prepared)  # Validate before any paid review.
        args.output.mkdir(parents=True, exist_ok=True)
        if views:
            prepare_nonportrait_views(views, args.output)
        destination = args.output / "prepared.png"
        if args.generate and destination.resolve() == args.input.resolve():
            parser.error("Generated output must not overwrite the original")
        (args.output / "prompt.txt").write_text(prompt, encoding="utf-8")
        rule_file = Path(__file__).with_name("image_preprocessing_prompts.py")
        offset = 0
        traced_layers = []
        for layer in layers:
            end = offset + len(layer["text"])
            traced_layers.append({**layer, "active": bool(layer["text"]), "start_char": offset, "end_char": end})
            offset = end
        prompt_trace = {
            "schema": "image-preprocessing-prompt-layers-v1", "subject": policy["subject"],
            "rules_file": str(rule_file.resolve()),
            "rules_sha256": hashlib.sha256(rule_file.read_bytes()).hexdigest(),
            "prompt_sha256": hashlib.sha256(prompt.encode("utf-8")).hexdigest(),
            "print_constraints_applied": bool(policy["specialized"] and policy["print_constraints"]),
            "layers": traced_layers,
        }
        (args.output / "prompt-layers.json").write_text(json.dumps(prompt_trace, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
        result = {"policy": policy, "original_quality": original_quality,
                  "prompt_sha256": prompt_trace["prompt_sha256"], "prompt_layers_file": "prompt-layers.json",
                  "image_edit_requested": args.generate, "semantic_review_requested": args.semantic_review,
                  "submits_3d_task": False}
        prepared = args.prepared
        if args.generate:
            prepared = preprocess_image(args.input, args.instruction, destination, (), args.style,
                                        custom_style=args.custom_style,
                                        print_settings=settings, preprocessing_options=options)
        if prepared and policy["specialized"]:
            result["reference_quality"] = review_nonportrait_reference(
                args.input, prepared, policy, args.output,
                completion=complete_vision_once if args.semantic_review else None,
                reviewer_model=os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4") if args.semantic_review else "",
            )
        (args.output / "preprocessing.json").write_text(json.dumps(result, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
        print(json.dumps({"output": str(args.output.resolve()), "subject": policy["subject"],
                          "image_edit_requested": args.generate, "submits_3d_task": False}, ensure_ascii=False))
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())

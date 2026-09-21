"""Summarize native recognizer evidence and replay actual ROIs against both SAMs.

Each model replay gets its own process so peak memory is not attributed to a
previous model. Quality scores and changed pixels are not ground-truth accuracy.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import numpy as np
from PIL import Image, ImageDraw

LABELS = ["unknown", "background", "hair", "face_skin", "body_skin", "clothes",
          "accessories", "lips", "mouth", "sclera", "iris", "eyebrow"]
COLORS = np.asarray([[110,110,110],[0,0,0],[40,70,190],[235,167,122],[198,144,110],
                     [40,185,70],[225,160,10],[220,40,80],[90,15,45],[240,240,240],
                     [30,145,150],[145,65,200]],dtype=np.uint8)

def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-ab", action="store_true")
    parser.add_argument("--per-part", type=int, default=2)
    parser.add_argument("--mobile-root", type=Path)
    parser.add_argument("--efficient-root", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    records, cases, selected, integrity_errors = [], [], {}, []
    for path in sorted(args.evidence_root.rglob("result.json")):
        doc=json.loads(path.read_text(encoding="utf-8-sig"))
        if not doc.get("schema","").startswith("orca.semantic-"):
            continue
        source=path.parent/"input.ppm"
        if not source.exists():
            continue
        image=Image.open(source).convert("RGB")
        record={"path":str(path.resolve()),"input_sha256":digest(source),**doc}
        if doc.get("schema")=="orca.semantic-render-evidence/v1":
            failures=[]
            for name, expected in doc.get("sha256",{}).items():
                saved=path.parent/name
                if not saved.is_file() or digest(saved)!=expected:
                    failures.append(f"Render evidence hash mismatch: {saved}")
            area=image.width*image.height
            for name, width in (("face_ids.u32",4),("depth.f32",4),("barycentric.f32",12)):
                saved=path.parent/name
                if not saved.is_file() or saved.stat().st_size!=area*width:
                    failures.append(f"Render evidence shape mismatch: {saved}")
            record["render_integrity_verified"]=bool(doc.get("sha256")) and not failures
            integrity_errors.extend(failures)
        elif doc.get("render_id"):
            rendered=path.parent.parent/doc["render_id"]/"input.ppm"
            record["render_input_matches"]=rendered.is_file() and digest(rendered)==record["input_sha256"]
            if not record["render_input_matches"]:
                integrity_errors.append(f"Recognizer input differs from observed render: {path}")
        label_path=path.parent/"labels.u8"
        key=hashlib.sha256(str(path.resolve()).encode()).hexdigest()[:12]
        if label_path.exists():
            labels=np.fromfile(label_path,dtype=np.uint8).reshape(image.height,image.width)
            if labels.max(initial=0)>=len(COLORS):
                raise ValueError(f"Unknown labels in {path}")
            mask=Image.fromarray(COLORS[labels]);mask.save(args.output/f"{key}-labels.png")
            overlay=Image.blend(image,mask,.45);draw=ImageDraw.Draw(overlay)
            for hint in doc.get("regions",[]):
                draw.rectangle(tuple(hint["box"]),outline="yellow")
                draw.text(tuple(hint["box"][:2]),f"person {hint['person_id']} part {hint['part']} side {hint['side']}",fill="white")
            overlay.save(args.output/f"{key}-overlay.png")
            record["overlay"]=f"{key}-overlay.png"
        prompt_path=path.parent/"prompts.json"
        if prompt_path.exists():
            prompts=json.loads(prompt_path.read_text())
            for region in prompts["regions"]:
                group=(str(path.parent.parent),region.get("part",0))
                if selected.get(group,0)>=args.per_part:
                    continue
                selected[group]=selected.get(group,0)+1
                case={"id":key,"input":str(source.resolve()),"prompts":str(prompt_path.resolve()),
                      "part":region.get("part"),"prompt_kind":"actual-native-automatic",
                      "native_error":doc.get("error",""),"native_rejected":doc.get("rejected",False),
                      "native_rejection_reason":doc.get("rejection_reason",""),
                      "native_model_score":doc.get("model_score")}
                if args.run_ab:
                    if not args.mobile_root or not args.efficient_root:
                        raise ValueError("Both model roots are required for --run-ab")
                    results=[]
                    for provider in ("mobile-sam","efficient-sam-tiny"):
                        out=args.output/key/provider
                        command=[sys.executable,str(Path(__file__).with_name("semantic_boundary_ab.py")),"--input",str(source),"--prompts",str(prompt_path),"--output",str(out),"--providers",provider]
                        if provider=="mobile-sam":
                            command += ["--mobile-encoder",str(args.mobile_root/"weights/mobile_sam_encoder.onnx"),"--mobile-decoder",str(args.mobile_root/"weights/mobile_sam_decoder.onnx")]
                        else:
                            command += ["--efficient-encoder",str(args.efficient_root/"weights/efficient_sam_vitt_encoder.onnx"),"--efficient-decoder",str(args.efficient_root/"weights/efficient_sam_vitt_decoder.onnx")]
                        out.mkdir(parents=True,exist_ok=True)
                        with (out/"run.log").open("w",encoding="utf8") as log:
                            completed=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=False)
                        if completed.returncode:
                            results.append({"provider":provider,"status":"error","log":str((out/"run.log").resolve())});continue
                        result=json.loads((out/"result-card.json").read_text())["results"][0]
                        result["result_card"]=str((out/"result-card.json").resolve())
                        if provider=="mobile-sam" and not doc.get("error") and not doc.get("rejected"):
                            native=np.fromfile(path.parent/"probability.f32",dtype=np.float32).reshape(image.height,image.width)
                            l,t,r,b=region["box"];native=native[t:b,l:r]
                            reference=np.load(out/provider/f"{region['id']}.soft-mask.npy")
                            if np.isfinite(native).all():
                                result["native_max_probability_delta"]=float(np.max(np.abs(native-reference)))
                                result["native_mask_mismatch_pixels"]=int(np.count_nonzero((native>=.5)!=(reference>=.5)))
                        results.append(result)
                    case["ab"]=results
                cases.append(case)
        records.append(record)
    summary={"schema":"orca.semantic-model-validation/v1","labels":LABELS,
             "notes":["Scores are model quality estimates, not measured accuracy.","Native masks precede semantic color guards and ROI acceptance.","No trusted reference contours: P95 and accuracy remain unmeasured."],
             "input_integrity_errors":integrity_errors,
             "recognizer_records":records,"replayed_cases":cases}
    (args.output/"model-validation.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf8")
    print(json.dumps({"records":len(records),"cases":len(cases),"integrity_errors":len(integrity_errors),"report":str((args.output/"model-validation.json").resolve())}))
    return 2 if integrity_errors else 0

if __name__=="__main__":
    raise SystemExit(main())

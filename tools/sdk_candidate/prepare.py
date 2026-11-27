"""Read three compact actual receipts only. Windows and execution inputs stay null."""
import argparse
from pathlib import Path
from adapter import SOURCE, TREE, STAGES, prepare_receipt, write


def prepare(evidence, output):
    base = Path(evidence) / 'normal-clean-build-b5f-v1'
    dirs = {
        'model-windows': ('model-windows-source-recovery-fix2-v1', 'key-seal.json'),
        'model-android': ('model-android-fix2-v1', 'key-seal.json'),
        'slicing-android': ('slicing-android-source-recovery-fix3-v1', 'actual-recovery-seal.json'),
    }
    producers = dict.fromkeys(STAGES)
    for stage, (directory, seal) in dirs.items():
        receipt = prepare_receipt(stage, base / directory, seal)
        receipt.update(resourceProof=None, sourceProof=None)
        producers[stage] = receipt
    plan = dict(state='PREPARED_UNBOUND_NOT_AUTHORIZED', source=SOURCE, tree=TREE,
                toolSource=None, sdkRepository=None, producers=producers,
                staging=None, output=None, evidence=None, debug=None,
                python=None, node=None, readobj=None, readelf=None, strip=None, ndk=None,
                ownerDirectory=None, toolFiles=[], stageInputs=[], postprocessProof=None,
                tbbSource=None, cgalArchive=None, dependencyManifest=None,
                functionalQualification=None)
    write(output, plan)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--parent-evidence', required=True)
    p.add_argument('--output', required=True)
    a = p.parse_args()
    prepare(a.parent_evidence, a.output)

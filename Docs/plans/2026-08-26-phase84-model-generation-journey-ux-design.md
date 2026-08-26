# Phase 84: Model Generation Journey UX Design

## Scope

This phase improves the internal-Beta journey after Phase 83 recovery hardening. It does not change providers, slicing algorithms, 3MF files, profiles, or paid retry policy.

## Progressive preview disclosure

The four comparison stages remain available because they are diagnostically valuable, but their visible names become task-oriented: AI design, filament colors, print-ready result, and changed areas. The default selection stays on the print-ready result. Plain-language guidance remains visible; pixel-level metrics move into a collapsed “processing details” section.

## Paid generation decision

The 3D confirmation states that one paid model task will be authorized, the current Provider account determines the monetary charge, generation usually takes several minutes, and stopping OrcaSlicer only stops local tracking when the remote Provider cannot cancel the task. The product does not hard-code a currency price that may be wrong across providers or plans.

## Performance evidence

OBJ parsing is timed locally with a monotonic clock. The model summary reports triangle count, dimensions, colors, and the measured parse duration. Models at or above 300,000 triangles receive a plain warning that preview and slicing may be slower. The metric is also stored in compatible library metadata.

## History and print feedback

Each history card exposes Load and Delete Local buttons; double-click remains a compatibility shortcut. Deletion requires confirmation and is constrained to the generated-model root. Successful imports update local metadata with import time and whether auto-slicing was requested. Internal testers may explicitly mark a model as “printed successfully” or “print had issues”; this is the only field presented as real print outcome because the current artifact-consumer interface does not receive printer completion events.

# Verification

Choose checks that demonstrate the changed behavior and satisfy project requirements. Add regression tests for meaningful risks; do not add tests that merely mirror low-impact code or wording. Reuse existing checks and stop repeating them after they pass unless new evidence justifies more.

For UI work, inspect the actual affected route and states using the available permitted viewer. Check failure recovery and state retention when changed. Fix observed defects within scope; distinguish local preview from deployment, and never deploy just to obtain a screenshot without applicable authorization.

For non-visual behavior, inspect relevant outputs and failure cases. Keep credentials and private payloads out of evidence. Report what was run, its result and material limits; unavailable GUI or real-service validation must remain unverified.

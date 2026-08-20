# Floating Component Printability Implementation Plan

## Goal

Warn when a generated OBJ contains clearly separated components that neither reach the print bed nor contact an already supported component, including components too large to trigger the existing tiny-component rule.

## Design

The deterministic model-quality gate will classify components conservatively from their axis-aligned bounds. Components inside the existing ground band are supported. Remaining components are processed bottom-up; a component is considered supported when its bounds are within a small contact tolerance of the aggregate bounds of already supported geometry. Only components with a clear positive gap become warning-only floating components.

Using aggregate supported bounds intentionally favors false negatives over false positives. Nested shells, overlapping material shells, duplicated OBJ seams, and components that plausibly touch existing geometry are not flagged by this rule.

## Tasks

1. Add regression fixtures for an equal-size floating component and for a separate shell within contact tolerance.
2. Add a stable `component_contact_tolerance_mm` threshold and floating-component metrics.
3. Emit `floating_disconnected_components` as `review` only; never add a new hard rejection.
4. Advance the report gate version while preserving schema version 1 and old-report parsing.
5. Add the model-generation GUI warning translation.
6. Run focused quality tests, Sidecar contract tests, all offline Python AI tests excluding the known readiness launcher timeout, Python syntax checks, and the Windows Release build.

## Boundary

This is local artifact analysis only. It does not move geometry, add supports, merge shells, modify OBJ/3MF/profile data, select slicing parameters, or call a Provider.

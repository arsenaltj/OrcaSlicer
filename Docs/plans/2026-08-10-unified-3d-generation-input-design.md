# Unified 3D Generation Input Design

## Goal

Move `3D Generate` before `Prepare`, remove the source-mode decision, reduce the crowded workflow column, and make printable colors both live and configurable without changing slicer project settings.

## Navigation

Add an explicit `tpGenerate3D` tab position after Home. Shift the existing enum positions and insert the generation page at that position. Existing callers continue using named enum values, so Prepare, Preview, Device and auxiliary tabs retain their behavioral identities.

## Unified input

The description and image picker are always visible. Input routing is derived from content:

- description only: preprocess text, review the prepared prompt, then generate 3D;
- image only: use a built-in printable style instruction, create an AI style preview, then generate 3D;
- description and image: use the description as the image-edit instruction, review the style preview, then generate 3D;
- neither: keep the primary action disabled.

The input used for a prepared job is snapshotted. Editing the description, replacing the image, or changing the palette invalidates the generation confirmation until a new preview is prepared.

## Compact workflow

Use one compact progress header and gauge at the top of the left column. Remove the mode dropdown and the permanently expanded three-section form. Keep input, palette and the current action visible. Show the prepared text only during text review, show Generate only while confirmation is pending, show Import only when the artifact is ready, and show Stop only during work. This keeps a single primary action at each state.

## Palette

Provide two sources:

- `Current project`: refresh from `Plater::get_extruder_colors_from_plater_config()` whenever the page is shown or controls refresh;
- `Custom for AI`: maintain up to 16 colors in the generation page, with a color picker and add/remove controls.

Custom colors affect only AI preview and OBJ color constraints. They do not modify `filament_colour`, printer presets, saved projects or slicing behavior. The effective palette is snapshotted when preprocessing starts and must still match before 3D generation.

## Verification

Build the Windows Release targets and run the AI Python suite. At 200% DPI verify tab order, compact layout, no clipping, automatic project colors, custom add/remove, and disabled/enabled actions for empty, text-only, image-only and combined inputs.

# Image comparison preview design

## Goal

The image-to-3D review step must show the uploaded reference and the AI-processed image at the same time, so the user can verify that styling occurred before approving a paid 3D generation call.

## Interaction

- The preview canvas uses two fixed panes: `Reference` on the left and `AI result` on the right.
- Before preprocessing, the reference remains visible and the result pane shows `AI result pending`.
- During preprocessing and download, the result pane shows a short loading state.
- On failure, the reference remains visible and the result pane shows `Preview unavailable`.
- On success, both images are shown with their full composition and pixel dimensions. Only then is `Generate 3D from preview` enabled.
- Fit, zoom in, zoom out, and scrolling apply to the comparison as one viewport.

## Rendering

- Reference and result images have independent source images and scaled bitmap caches.
- At Fit, each image is scaled independently into half of the available canvas while preserving aspect ratio.
- At higher zoom levels, both panes expand and the existing scrolled window provides navigation.
- Pane dimensions and label height remain stable across pending, loading, failure, and success states.

## Verification

- Build `libslic3r_gui` and `OrcaSlicer` in Windows Release mode.
- Use the mock sidecar to enter image review without a paid provider call.
- Verify both pane labels and images are visible at Fit, the dimensions summary contains both inputs, and 3D generation is enabled only after the result is loaded.
- Verify shared zoom reaches 125% and Fit restores 100%.

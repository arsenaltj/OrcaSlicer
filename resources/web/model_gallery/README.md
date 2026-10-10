# Installed model gallery

The desktop Assets page loads this entry page lazily. Catalog metadata and the 43 small previews are a snapshot of the user's public gallery, <https://arsenaltj.github.io/>, originally prepared on 2026-09-25. The previews total about 1.2 MB; full model files and private download allowlists are not bundled.

Browsing, searching, filtering, sorting and details use local resources. Download links open in the default browser; model service health is checked with a bounded, credential-free request, and an unconfirmed status is not shown as success. Flat images use their public image URL. The page has no application command or filesystem bridge.

The layout borrows the category/search/card browsing pattern of MakerWorld while using the Orca redesign theme. It does not import MakerWorld assets, account features, rankings or usage metrics.

Run the catalog, filtering and download-link checks with `node --test tests/web/test_model_gallery.cjs` from the repository root. Native navigation policy is covered by the existing `AssetsWorkspace` CTest label; real WebView and journey behavior require the matching Windows GUI.

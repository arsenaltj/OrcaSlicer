#pragma once

#include "RedesignTheme.hpp"
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <optional>

namespace Slic3r::GUI {

struct HonorAccountTrustCopy {
    const char* source;
    const char* replacement;
};

inline constexpr std::array<HonorAccountTrustCopy, 2> honor_account_trust_copy()
{
    return {{
        {"是否信任此浏览器？", "是否信任 Honor Forge Studio？"},
        {"若信任，当您下次登录时，系统将不会要求您再次验证。",
         "若信任该软件，您下次登录时将不再需要重复验证。"},
    }};
}

inline float honor_account_zoom_factor(const wxSize& viewport_dip)
{
    if (viewport_dip.x <= 0 || viewport_dip.y <= 0) return 1.0f;
    constexpr float design_width = 1600.0f;
    constexpr float design_height = 900.0f;
    constexpr float minimum_zoom = 0.65f;
    const float fit = std::min(viewport_dip.x / design_width,
                               viewport_dip.y / design_height);
    return std::clamp(fit, minimum_zoom, 1.0f);
}

inline std::optional<float> honor_account_zoom_factor_if_ready(bool browser_ready,
                                                                const wxSize& viewport_dip)
{
    if (!browser_ready) return std::nullopt;
    return honor_account_zoom_factor(viewport_dip);
}

// Presentation only: no native bridge, form values, event handlers, storage or
// requests. HONOR's current PC portal forces blue even with themeName=dark.
inline wxString honor_account_theme_script()
{
    wxString css = R"CSS(
:root[data-orca-honor-theme] {
    color-scheme: dark;
    --orca-panel: @PANEL@;
    --orca-control: @CONTROL@;
    --orca-text: @TEXT@;
    --orca-secondary: @SECONDARY@;
    --orca-accent: @ACCENT@;
    --orca-divider: @DIVIDER@;
    background: var(--orca-panel) !important;
}
:root[data-orca-honor-theme] body {
    --hnr-color-primary: var(--orca-text);
    --hnr-color-accent: var(--orca-accent);
    --hnr-color-background-cardview: var(--orca-panel);
    --font-text-color: var(--orca-text);
    --font-text-color-weak: var(--orca-secondary);
    --font-text-color-gray: var(--orca-secondary);
    --main-bg-color: var(--orca-panel);
    --primary-color: var(--orca-accent);
    --link-color: var(--orca-accent);
    --input-color: var(--orca-text);
    --input-placeholder-color: var(--orca-secondary);
    --dialog-bg-color: var(--orca-panel);
    --default-button-bg: var(--orca-control);
    --toast-bg-color: var(--orca-control);
    --bg-5: rgba(255,255,255,.05);
    --bg-10: rgba(255,255,255,.10);
    --bg-20: rgba(255,255,255,.20);
    --bg-30: rgba(255,255,255,.30);
    --bg-50: rgba(255,255,255,.50);
    --bg-60: rgba(255,255,255,.60);
    --bg-100: #fff;
    --border-color: var(--orca-divider);
    background: var(--orca-panel) !important;
    color: var(--orca-text) !important;
    font-family: "HONOR Sans Design", "Microsoft YaHei UI", sans-serif;
}
:root[data-orca-honor-theme],
:root[data-orca-honor-theme] body {
    scrollbar-width: none !important;
    -ms-overflow-style: none !important;
}
:root[data-orca-honor-theme]::-webkit-scrollbar,
:root[data-orca-honor-theme] body::-webkit-scrollbar {
    display: none !important;
    width: 0 !important;
    height: 0 !important;
}
:root[data-orca-honor-theme] #root,
:root[data-orca-honor-theme] .login-title {
    color: var(--orca-text) !important;
}
:root[data-orca-honor-theme] .customer-header,
:root[data-orca-honor-theme] .customer-footer,
:root[data-orca-honor-theme] .cookie-banner-box {
    background: var(--orca-control) !important;
    color: var(--orca-secondary) !important;
}
:root[data-orca-honor-theme] .customer-header img.logo {
    filter: brightness(0) invert(1);
}
:root[data-orca-honor-theme] .hnid-input {
    background: var(--orca-control) !important;
    color: var(--orca-text) !important;
    caret-color: var(--orca-accent);
    border: 1px solid var(--orca-divider);
    border-radius: 8px !important;
}
:root[data-orca-honor-theme] .hnid-input::placeholder {
    color: var(--orca-secondary) !important;
    opacity: 1;
}
:root[data-orca-honor-theme] .hnid-input:focus {
    border-color: var(--orca-accent) !important;
    box-shadow: 0 0 0 1px var(--orca-accent);
}
:root[data-orca-honor-theme] .hnid-input.error {
    border-color: #ff6b6b !important;
}
:root[data-orca-honor-theme] .eye-icon,
:root[data-orca-honor-theme] .cookie-banner-close {
    filter: invert(.85);
}
:root[data-orca-honor-theme] .hnid-button.hnid-button-primary {
    background: var(--orca-accent) !important;
    color: var(--orca-control) !important;
    border-radius: 8px !important;
}
:root[data-orca-honor-theme] .hnid-button.hnid-button-primary:hover {
    background: #ffd15b !important;
}
:root[data-orca-honor-theme] .hnid-button.hnid-button-primary:disabled,
:root[data-orca-honor-theme] .hnid-button.hnid-button-primary.hnid-button-disabled {
    background: rgba(255,194,39,.25) !important;
    color: rgba(255,255,255,.55) !important;
}
:root[data-orca-honor-theme] .hnid-button-link,
:root[data-orca-honor-theme] .hnid-link,
:root[data-orca-honor-theme] .cookie-banner-box a {
    color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .customer-footer .rule,
:root[data-orca-honor-theme] .cookie-banner-text,
:root[data-orca-honor-theme] .qrcode-desc {
    color: var(--orca-secondary) !important;
}
:root[data-orca-honor-theme] .customer-footer .rule:hover {
    color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .login-split-line {
    border-color: var(--orca-divider) !important;
}
/* Keep the original QR image, contrast, expiry overlay and captcha surfaces. */
:root[data-orca-honor-theme] .qrcode-box {
    background: #fff !important;
}
:root[data-orca-honor-theme] .hnid-select-list-wrap.default-theme {
    background: var(--orca-control) !important;
    border-color: var(--orca-divider) !important;
}
/* Portal dialogs mount under body as well as #root. Keep their tokens local:
   changing body-wide library tokens would also repaint the accepted login page.
   CSS also covers later mounts without observing forms or changing handlers. */
:root[data-orca-honor-theme] :is(.ant-modal, .hnui-dialog, .hnr-dialog) {
    --hnr-dialog-background: var(--orca-panel);
    --hnr-dialog-color: var(--orca-text);
    --hnr-dialog-button-color: var(--orca-accent);
    --hnr-text-color-primary: var(--orca-text);
    --hnr-text-color-secondary: var(--orca-secondary);
    --hnr-text-color-hint: var(--orca-secondary);
    --hnr-text-color-link: var(--orca-accent);
    --hnr-color-primary: var(--orca-text);
    --hnr-color-accent: var(--orca-accent);
    --hnr-button-primary-background: var(--orca-accent);
    --confirm-btn-color: var(--orca-accent);
    color: var(--orca-text);
}
:root[data-orca-honor-theme] :is(.ant-modal-content, .hnui-dialog-main, .hnr-dialog) {
    background: var(--orca-panel) !important;
    color: var(--orca-text) !important;
    box-shadow: 0 12px 40px rgba(0,0,0,.35), inset 0 0 0 1px var(--orca-divider);
}
:root[data-orca-honor-theme] .ant-modal :is(.ant-modal-header, .ant-modal-body, .ant-modal-footer) {
    background: var(--orca-panel) !important;
    color: var(--orca-text) !important;
    border-color: var(--orca-divider) !important;
}
:root[data-orca-honor-theme] :is(.ant-modal-title, .ant-modal-confirm-title, .ant-modal-confirm-content,
    .hnui-modal-header, .hnui-dialog-title, .hnr-dialog__header, .hnr-dialog__message) {
    color: var(--orca-text) !important;
}
:root[data-orca-honor-theme] .ant-modal :is(.ant-form-item-label > label, .ant-checkbox-wrapper,
    .ant-radio-wrapper, .ant-modal-close) {
    color: var(--orca-text) !important;
}
:root[data-orca-honor-theme] :is(.ant-modal, .hnui-dialog, .hnr-dialog)
    :is(.hnid-button-default, .ant-btn-default, .hnr-button--default):not(.ant-btn-dangerous, .hnr-button--danger, .hnr-button--warning) {
    background: var(--orca-control) !important;
    color: var(--orca-text) !important;
    border-color: var(--orca-divider) !important;
}
:root[data-orca-honor-theme] :is(.ant-modal, .hnui-dialog, .hnr-dialog)
    :is(.hnid-button-primary, .ant-btn-primary, .hnr-button--primary):not(.ant-btn-dangerous, .hnr-button--danger, .hnr-button--warning) {
    background: var(--orca-accent) !important;
    color: var(--orca-control) !important;
    border-color: transparent !important;
}
:root[data-orca-honor-theme] :is(.ant-modal, .hnui-dialog, .hnr-dialog)
    :is(.hnid-button-primary, .ant-btn-primary, .hnr-button--primary):hover:not(.ant-btn-dangerous, .hnr-button--danger, .hnr-button--warning):not(:disabled):not([disabled]):not([aria-disabled="true"]):not(.hnid-button-disabled):not(.hnr-button--disabled):not(.loading):not(.ant-btn-loading) {
    background: #ffd15b !important;
}
:root[data-orca-honor-theme] :is(.ant-modal, .hnui-dialog, .hnr-dialog)
    :is(.hnid-button-link, .ant-btn-link, .ant-btn-text, .hnid-link, .dialog-btn):not(.ant-btn-dangerous, .hnr-button--danger, .hnr-button--warning) {
    color: var(--orca-accent) !important;
    background: transparent !important;
}
/* Paint disabled/loading states; their attributes, opacity, hit testing and
   countdown text remain owned by HONOR. Do not make disabled hover look ready. */
:root[data-orca-honor-theme] :is(.ant-modal, .hnui-dialog, .hnr-dialog)
    :is(.hnid-button-primary, .ant-btn-primary, .hnr-button--primary):not(.ant-btn-dangerous, .hnr-button--danger, .hnr-button--warning):is(:disabled, [disabled], [aria-disabled="true"], .hnid-button-disabled, .hnr-button--disabled, .loading, .ant-btn-loading) {
    background: rgba(255,194,39,.25) !important;
    color: rgba(255,255,255,.55) !important;
}
:root[data-orca-honor-theme] :is(.ant-modal, .hnui-dialog, .hnr-dialog)
    :is(.hnid-button-default, .ant-btn-default, .hnr-button--default, .hnid-button-link, .ant-btn-link, .ant-btn-text, .dialog-btn):not(.ant-btn-dangerous, .hnr-button--danger, .hnr-button--warning):is(:disabled, [disabled], [aria-disabled="true"], .hnid-button-disabled, .hnr-button--disabled, .loading, .ant-btn-loading) {
    color: var(--orca-secondary) !important;
}
:root[data-orca-honor-theme] :is(.ant-modal, .hnui-dialog, .hnr-dialog)
    :is(.hnid-button, .ant-btn, .hnr-button, .dialog-btn):focus-visible {
    box-shadow: 0 0 0 2px var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .ant-modal :is(.ant-input, .ant-input-affix-wrapper) {
    background: var(--orca-control) !important;
    color: var(--orca-text) !important;
    caret-color: var(--orca-accent);
}
:root[data-orca-honor-theme] .ant-modal .ant-input::placeholder {
    color: var(--orca-secondary) !important;
}
:root[data-orca-honor-theme] .ant-modal :is(.ant-input, .ant-input-affix-wrapper):focus-within {
    border-color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .ant-modal :is(.ant-input-status-error, .ant-input-affix-wrapper-status-error,
    .ant-form-item-has-error .ant-input, .ant-form-item-has-error .ant-input-affix-wrapper) {
    border-color: #ff6b6b !important;
}
:root[data-orca-honor-theme] :is(.hnui-toast-main, .hwid-common-toast .toast, .toast-wrap,
    .ant-message-notice-content, .ant-notification-notice) {
    background: var(--orca-control) !important;
    color: var(--orca-text) !important;
}
/* Registration has its own hard-coded light palette. Scope these overrides to
   its verified container; retain official geometry, validation and events. */
:root[data-orca-honor-theme] .hnid-root:has(.hnid-register-main),
:root[data-orca-honor-theme] .hnid-register-main :is(.hnid-mianTitle, .hnid-reg-login-link,
    .hnid-country-left, .hnid-country-right, .hnid-register-menustyle) {
    color: var(--orca-text) !important;
}
:root[data-orca-honor-theme] .hnid-register-main {
    background-color: var(--orca-panel) !important;
}
:root[data-orca-honor-theme] .hnid-register-main :is(.hnid-reg-link, .portal-hnid-link, .get-auth-code) {
    color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .hnid-right-arrow {
    border-color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .hnid-icon-color-fill {
    fill: var(--orca-secondary) !important;
    color: var(--orca-secondary) !important;
}
:root[data-orca-honor-theme] .hnid-register-main :is(.vue-dropdown.default-theme, .hnid-ddrop, .hnid-drop-menu) {
    background-color: var(--orca-control) !important;
    color: var(--orca-text) !important;
    border-color: var(--orca-divider) !important;
}
:root[data-orca-honor-theme] .hnid-register-main :is(.search-module, .input-div, .search-text) {
    background-color: var(--orca-control) !important;
    color: var(--orca-text) !important;
    border-color: var(--orca-divider) !important;
    caret-color: var(--orca-accent);
}
:root[data-orca-honor-theme] .hnid-register-main .search-text::placeholder {
    color: var(--orca-secondary) !important;
}
:root[data-orca-honor-theme] .hnid-register-main :is(.search-text-search, .search-text-close) {
    filter: invert(.85);
}
:root[data-orca-honor-theme] .hnid-register-main :is(.orderItem, .hwid-choose-country,
    .hwid-common-line, .flow-label, .line-container-nav, .li-wrap) {
    color: var(--orca-text) !important;
}
:root[data-orca-honor-theme] .hnid-register-main :is(.orderItem, .li-wrap):hover {
    background-color: rgba(255,194,39,.12) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .orderItem:hover {
    color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .input-div:focus-within {
    border-color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .hnid-country-drop-down:has(input.hnid-input:focus) {
    border-color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .hnid-drop-splitLine {
    background-color: var(--orca-divider) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .hwid-common-line {
    background-color: var(--orca-divider) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .line-container-nav {
    background-color: rgba(255,255,255,.05) !important;
}
:root[data-orca-honor-theme] .hnid-register-main .get-auth-code:is(:disabled, [disabled], [aria-disabled="true"], .ant-btn-loading) {
    color: var(--orca-secondary) !important;
}
:root[data-orca-honor-theme] .hnid-register-main :is(.hnid-reg-link, .get-auth-code, .register-button):focus-visible {
    outline-color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme]:has(.hnid-register-main) .customer-footer :is(.copy-right, .case-number, a) {
    color: var(--orca-secondary) !important;
}
/* The verified OAuth confirmation page uses page_honor.css rather than CAS
   components, including an inline white panel and image-based checkboxes.
   Keep its geometry, hidden inputs, selection state and handlers untouched. */
:root[data-orca-honor-theme] body > div:has(> #acc-app-authorize),
:root[data-orca-honor-theme] #acc-app-authorize > div:has(> #apphead-logo),
:root[data-orca-honor-theme] #acc-app-authorize .acc-box {
    background-color: var(--orca-panel) !important;
}
:root[data-orca-honor-theme] #acc-app-authorize,
:root[data-orca-honor-theme] #acc-app-authorize :is(#authorization_text, #selectAll, .rights-option, .acc-topbar .title) {
    color: var(--orca-text) !important;
}
:root[data-orca-honor-theme] #acc-app-authorize :is(.scope-desc, .privacy-state, .norights-tips) {
    color: var(--orca-secondary) !important;
    opacity: 1;
}
:root[data-orca-honor-theme] #acc-app-authorize #apphead-logo {
    border-bottom-color: var(--orca-divider) !important;
}
:root[data-orca-honor-theme] #acc-app-authorize .uc-img {
    filter: brightness(0) invert(1);
}
/* The current app mark is monochrome on white JPEG; screen blends out its
   inverted black backing without recoloring any other page images. */
:root[data-orca-honor-theme] #acc-app-authorize #apphead-logo img.logo {
    filter: invert(1);
    mix-blend-mode: screen;
}
:root[data-orca-honor-theme] #acc-app-authorize :is(#selectAll img, .rights-img) {
    filter: invert(1) sepia(1) saturate(3);
}
:root[data-orca-honor-theme] #acc-app-authorize #ok-btn {
    background-color: var(--orca-accent) !important;
    color: var(--orca-control) !important;
}
/* HONOR disables this anchor by removing href and setting inline opacity.
   Preserve both; a disabled authorization action must not gain a hover state. */
:root[data-orca-honor-theme] #acc-app-authorize #ok-btn[href]:not([aria-disabled="true"]):hover {
    background-color: #ffd15b !important;
}
:root[data-orca-honor-theme] #acc-app-authorize #ok-btn[href]:not([aria-disabled="true"]):active {
    background-color: #e8ad19 !important;
}
:root[data-orca-honor-theme] #acc-app-authorize #cancel-btn {
    background-color: var(--orca-control) !important;
    color: var(--orca-text) !important;
}
:root[data-orca-honor-theme] #acc-app-authorize #cancel-btn:hover {
    background-color: rgba(255,255,255,.10) !important;
}
:root[data-orca-honor-theme] #acc-app-authorize :is(#ok-btn, #cancel-btn):focus-visible {
    outline: 2px solid var(--orca-accent);
    outline-offset: 2px;
}
:root[data-orca-honor-theme]:has(#acc-app-authorize) #pageHuaweiFooter,
:root[data-orca-honor-theme]:has(#acc-app-authorize) #pageHuaweiFooter :is(.ft, .rule, #caseNumber, .case-number-desc, .copyright, .foot_em) {
    color: var(--orca-secondary) !important;
}
:root[data-orca-honor-theme]:has(#acc-app-authorize) #pageHuaweiFooter .copyright {
    opacity: 1;
}
:root[data-orca-honor-theme]:has(#acc-app-authorize) #pageHuaweiFooter a:hover {
    color: var(--orca-accent) !important;
}
:root[data-orca-honor-theme]:has(#acc-app-authorize) #pageHuaweiFooter .case-img {
    filter: invert(.85);
}
)CSS";
    using namespace RedesignTheme;
    css.Replace("@PANEL@", panel_colour().GetAsString(wxC2S_CSS_SYNTAX));
    css.Replace("@CONTROL@", control_colour().GetAsString(wxC2S_CSS_SYNTAX));
    css.Replace("@TEXT@", primary_text_colour().GetAsString(wxC2S_CSS_SYNTAX));
    css.Replace("@SECONDARY@", secondary_text_colour().GetAsString(wxC2S_CSS_SYNTAX));
    css.Replace("@ACCENT@", accent_colour().GetAsString(wxC2S_CSS_SYNTAX));
    css.Replace("@DIVIDER@", divider_colour().GetAsString(wxC2S_CSS_SYNTAX));
    nlohmann::json trust_copy = nlohmann::json::object();
    for (const auto& item : honor_account_trust_copy())
        trust_copy[item.source] = item.replacement;
    std::string script = R"JS((function () {
    'use strict';
    if (window !== window.top || location.protocol !== 'https:' ||
        (location.port && location.port !== '443')) return;
    const cas = location.hostname === 'hnid-test-drcn.cloud.honor.com' &&
        location.pathname.startsWith('/CAS/portal/');
    const oauth = ['hnoauth-login-test-drcn.cloud.honor.com', 'hnoauth-login.cloud.honor.com']
        .includes(location.hostname) && /^\/oauth2\/v3\/(authorize|login)$/.test(location.pathname);
    if (!cas && !oauth) return;
    if (window.__orcaHonorPresentationRefresh) {
        window.__orcaHonorPresentationRefresh();
        return;
    }
    const css = )JS" + nlohmann::json(css.ToStdString(wxConvUTF8)).dump() + R"JS(;
    const trustCopy = )JS" + trust_copy.dump() + R"JS(;
    const trustDialogSelector = [
        '[ht="pop-trust-browser-dialog"]',
        '.pop-trust-browser-dialog',
        '[data-testid="trust-browser-container"]',
        '.trust-browser',
        '[role="dialog"]',
        '.ant-modal',
        '.hnui-dialog',
        '.hnr-dialog'
    ].join(',');
    const rewriteTrustCatalog = function () {
        const catalog = window.rss_new;
        if (!catalog || typeof catalog !== 'object') return;
        for (const key of [
            'trustBrowser_label_DialogTitle',
            'trustBrowser_label_content',
            'trustBrowser_label_content_new'
        ]) {
            if (typeof catalog[key] !== 'string') continue;
            let next = catalog[key];
            for (const [source, replacement] of Object.entries(trustCopy))
                next = next.replace(source, replacement);
            if (next !== catalog[key]) catalog[key] = next;
        }
    };
    const rewriteText = function (root) {
        const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
        for (let node = walker.nextNode(); node; node = walker.nextNode()) {
            let next = node.nodeValue;
            for (const [source, replacement] of Object.entries(trustCopy))
                next = next.replace(source, replacement);
            if (next !== node.nodeValue) node.nodeValue = next;
        }
    };
    const rewriteTrustMarker = function (marker) {
        if (!marker || marker.nodeType !== Node.ELEMENT_NODE) return;
        // HONOR renders the title beside the data-testid content node in one
        // portal variant. Walk only its nearby dialog ancestors.
        let scope = marker;
        for (let depth = 0; scope && scope !== document.body && depth < 6; ++depth) {
            rewriteText(scope);
            const text = scope.textContent || '';
            if (Object.values(trustCopy).every(value => text.includes(value))) break;
            scope = scope.parentElement;
        }
    };
    const rewriteTrustCopy = function (root) {
        rewriteTrustCatalog();
        if (!root) return;
        if (root.nodeType === Node.ELEMENT_NODE && root.matches(trustDialogSelector))
            rewriteTrustMarker(root);
        if (root.querySelectorAll)
            for (const marker of root.querySelectorAll(trustDialogSelector))
                rewriteTrustMarker(marker);
    };
    const install = function (target) {
        const documentRoot = document.documentElement;
        if (!documentRoot) return false;
        documentRoot.setAttribute('data-orca-honor-theme', '1');
        if (!document.getElementById('orca-honor-theme')) {
            const style = document.createElement('style');
            style.id = 'orca-honor-theme';
            style.textContent = css;
            documentRoot.appendChild(style);
        }
        rewriteTrustCopy(target || document);
        return true;
    };
    let scheduled = false;
    const pendingRoots = new Set();
    const schedule = function (root) {
        if (root) pendingRoots.add(root.nodeType === Node.TEXT_NODE ? root.parentElement : root);
        if (scheduled) return;
        scheduled = true;
        queueMicrotask(function () {
            scheduled = false;
            const roots = Array.from(pendingRoots);
            pendingRoots.clear();
            rewriteTrustCatalog();
            for (const candidate of roots) rewriteTrustCopy(candidate);
        });
    };
    window.__orcaHonorPresentationRefresh = function () { install(document); };
    const observer = new MutationObserver(function (records) {
        for (const record of records) {
            if (record.type === 'characterData') {
                const marker = record.target.parentElement && record.target.parentElement.closest(trustDialogSelector);
                if (marker) schedule(marker);
                continue;
            }
            for (const node of record.addedNodes) {
                const element = node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement;
                if (element && (element.matches(trustDialogSelector) ||
                    (element.querySelector && element.querySelector(trustDialogSelector)))) schedule(element);
            }
        }
    });
    observer.observe(document, {childList: true, subtree: true, characterData: true});
    window.__orcaHonorPresentationObserver = observer;
    install(document);
})();)JS";
    return wxString::FromUTF8(script);
}

inline wxString honor_account_submit_watchdog_script()
{
    // This bridge reports only that the owning page submitted a form. It does
    // not read fields, block submission, inspect storage, or perform requests.
    return wxString::FromUTF8(R"JS((function () {
    'use strict';
    if (window !== window.top || location.protocol !== 'https:' ||
        (location.port && location.port !== '443') || window.__orcaHonorSubmitWatchdog) return;
    const cas = location.hostname === 'hnid-test-drcn.cloud.honor.com' &&
        location.pathname.startsWith('/CAS/portal/');
    const oauth = ['hnoauth-login-test-drcn.cloud.honor.com', 'hnoauth-login.cloud.honor.com']
        .includes(location.hostname) && /^\/oauth2\/v3\/(authorize|login)$/.test(location.pathname);
    if (!cas && !oauth) return;
    window.__orcaHonorSubmitWatchdog = true;
    document.addEventListener('submit', function () {
        if (window.honorAuthSubmit && typeof window.honorAuthSubmit.postMessage === 'function')
            window.honorAuthSubmit.postMessage('honor-submit-v1');
    }, true);
})();)JS");
}

} // namespace Slic3r::GUI

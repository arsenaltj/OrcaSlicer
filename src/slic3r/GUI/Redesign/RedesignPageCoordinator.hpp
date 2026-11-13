#pragma once
#include "RedesignWorkspaceNavigation.hpp"
#include "RedesignPageTransition.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace Slic3r::GUI {

// Owns route and retained-page lifetime, never generation/editing state.
class RedesignPageCoordinator {
public:
    using Page = RedesignWorkspacePage;
    using View = RedesignModelView;
    enum class Lifetime { Uncreated, Hidden, Preparing, Visible, Disposing, Disposed };
    struct Adapter {
        std::function<void()> prepare, apply_snapshot, presented, hidden, dispose;
        Lifetime lifetime {Lifetime::Uncreated};
    };
    Page page() const { return m_page; }
    View view() const { return m_view; }
    bool advanced() const { return m_advanced; }
    std::uint64_t epoch() const { return m_epoch; }
    RedesignPageTransition& transition() { return m_transition; }
    const RedesignPageTransition& transition() const { return m_transition; }
    void select_page(Page page) { if (m_page != page) { m_page = page; changed(); } }
    void select_view(View view) { if (m_view != view) { m_view = view; changed(); } }
    void select_mode(bool advanced) { if (m_advanced != advanced) { m_advanced = advanced; changed(); } }
    static std::size_t key(Page page, View view = View::Result, bool advanced = false) {
        switch (page) {
        case Page::Assets: return 0;
        case Page::Image: return advanced ? 2 : 1;
        case Page::Model: return 3 + static_cast<std::size_t>(view);
        case Page::Print: return 7;
        case Page::Account: return 8;
        }
        return 8;
    }
    void register_page(Page page, View view, bool advanced, Adapter adapter) {
        auto& slot = m_pages[key(page, view, advanced)];
        if (slot.lifetime != Lifetime::Uncreated) return;
        adapter.lifetime = Lifetime::Hidden; slot = std::move(adapter);
    }
    void prepare_current() {
        auto& target = m_pages[current_key()];
        if (target.lifetime != Lifetime::Hidden) return;
        target.lifetime = Lifetime::Preparing;
        if (target.prepare) target.prepare();
    }
    void presented_current() {
        const auto key = current_key();
        auto& target = m_pages[key];
        if (target.lifetime == Lifetime::Disposed || target.lifetime == Lifetime::Uncreated) return;
        if (m_presented && *m_presented != key) {
            auto& previous = m_pages[*m_presented];
            previous.lifetime = Lifetime::Hidden;
            if (previous.hidden) previous.hidden();
        }
        const bool newly_visible = !m_presented || *m_presented != key || target.lifetime != Lifetime::Visible;
        m_presented = key; target.lifetime = Lifetime::Visible;
        if (target.apply_snapshot) target.apply_snapshot();
        if (newly_visible && target.presented) target.presented();
    }
    void dispose() {
        ++m_epoch;
        for (auto& page : m_pages) {
            if (page.lifetime == Lifetime::Uncreated || page.lifetime == Lifetime::Disposed) continue;
            const bool visible = page.lifetime == Lifetime::Visible;
            page.lifetime = Lifetime::Disposing;
            if (visible && page.hidden) page.hidden();
            if (page.dispose) page.dispose();
            page = {}; page.lifetime = Lifetime::Disposed;
        }
        m_presented.reset();
    }
    Lifetime lifetime(Page page, View view = View::Result, bool advanced = false) const {
        return m_pages[key(page, view, advanced)].lifetime;
    }
private:
    void changed() {
        ++m_epoch;
        // Rollback/navigation cannot leave an abandoned adapter preparing.
        // A retained source remains visible until the target commits.
        for (std::size_t i = 0; i < m_pages.size(); ++i) {
            auto& page = m_pages[i];
            if (i != current_key() && page.lifetime == Lifetime::Preparing) {
                page.lifetime = Lifetime::Hidden;
                if (page.hidden) page.hidden();
            }
        }
    }
    std::size_t current_key() const { return key(m_page, m_view, m_advanced); }
    Page m_page {Page::Account};
    View m_view {View::Result};
    bool m_advanced {false};
    std::uint64_t m_epoch {0};
    std::array<Adapter, 9> m_pages;
    std::optional<std::size_t> m_presented;
    RedesignPageTransition m_transition;
};
} // namespace Slic3r::GUI

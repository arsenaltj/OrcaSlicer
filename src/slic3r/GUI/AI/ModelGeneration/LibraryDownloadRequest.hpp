#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace Slic3r::GUI {
struct LibraryDownloadRequest {
    enum class Phase { Checking, Transfer, Verify, WaitingForAccount, Registering, Complete, Failed, Cancelled };

    std::string key, file;
    std::uint64_t request;
    std::size_t done {0}, total {0};
    std::uint64_t registration_scan {0}, update {0};
    Phase phase {Phase::Checking};
    bool retried {false}, login_required {false};
    std::string message;

    bool in_worker() const { return phase == Phase::Checking || phase == Phase::Transfer || phase == Phase::Verify; }
    bool cancelable() const { return in_worker() || phase == Phase::WaitingForAccount; }
    bool active() const { return cancelable() || phase == Phase::Registering; }

    void fail(std::string reason, bool login = false)
    {
        phase = Phase::Failed;
        login_required = login;
        message = std::move(reason);
    }

    void credentials_changed(bool can_recover)
    {
        if (!cancelable()) return;
        if (!can_recover) fail("平台登录已失效，请重新登录。", true);
        else if (in_worker()) {
            if (retried) fail("平台登录再次刷新，下载已暂停，请重试。");
            else {
                retried = true;
                phase = Phase::WaitingForAccount;
            }
        }
    }

    // Only a new usable lease may restart an interrupted transfer.
    bool resume()
    {
        if (phase != Phase::WaitingForAccount) return false;
        phase = Phase::Checking;
        done = 0;
        return true;
    }
};
}

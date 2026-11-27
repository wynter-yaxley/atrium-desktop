#include "ui/notifications.h"

#include "ui/notification_layout.h"

#include <algorithm>

namespace atrium {

namespace notification {

bool HasExternalId(const Notice& notice, uint32_t externalId)
{
    if (!externalId)
        return false;
    return notice.externalId == externalId || std::ranges::find(notice.externalIds, externalId) != notice.externalIds.end();
}

void AddExternalId(Notice& notice, uint32_t externalId)
{
    if (!externalId)
        return;
    if (!notice.externalId)
        notice.externalId = externalId;
    else if (notice.externalId != externalId && std::ranges::find(notice.externalIds, notice.externalId) == notice.externalIds.end())
        notice.externalIds.push_back(notice.externalId);
    if (std::ranges::find(notice.externalIds, externalId) == notice.externalIds.end())
        notice.externalIds.push_back(externalId);
}

bool WithdrawNotice(Notice& existing, const Notice& withdrawal)
{
    if (existing.source != withdrawal.source)
        return false;
    if (!withdrawal.externalId)
        return existing.title == withdrawal.title && (!withdrawal.pid || !existing.pid || existing.pid == withdrawal.pid);
    if (!HasExternalId(existing, withdrawal.externalId))
        return false;
    std::erase(existing.externalIds, withdrawal.externalId);
    if (existing.externalId == withdrawal.externalId)
        existing.externalId = existing.externalIds.empty() ? 0 : existing.externalIds.front();
    return existing.externalIds.empty() && !existing.externalId;
}

} // namespace notification

void Notifications::ApplyAlways(bool notifyAlways)
{
    const bool changed = always_ != notifyAlways;
    always_ = notifyAlways;
    ArmPopupTimer();
    if (changed && popup_.surface && popup_.surface->Visible())
        Layout(Mode::Popup);
}

void Notifications::SetDoNotDisturb(bool on)
{
    if (dnd_ == on)
        return;
    dnd_ = on;
    if (dnd_)
        HidePopup(L"dnd");
}

void Notifications::ArmPopupTimer()
{
    if (!popup_.surface)
        return;
    if (always_) {
        KillTimer(popup_.surface->Hwnd(), kPopupTimer);
        return;
    }
    if (popup_.surface->Visible() && !popup_.tracking)
        SetTimer(popup_.surface->Hwnd(), kPopupTimer, UINT(timeout_ * 1000), nullptr);
}

void Notifications::OnPopupTimer()
{
    if (always_) {
        KillTimer(popup_.surface->Hwnd(), kPopupTimer);
        return;
    }
    HidePopup(L"timeout");
}

bool Notifications::ShowControls(Mode mode) const { return mode == Mode::Center || always_; }

bool Notifications::CanCollapse(Mode mode, bool column) const { return mode == Mode::Center && !column; }

size_t Notifications::LiveCount(Mode mode, size_t shownCount) const
{
    const size_t total = mode == Mode::Popup ? notices_.size() : shownCount;
    return total - std::min(total, leaving_.size());
}

void Notifications::Withdraw(const Notice& withdrawal)
{
    std::vector<uint64_t> ids;
    for (Notice& o : notices_)
        if (notification::WithdrawNotice(o, withdrawal))
            ids.push_back(o.id);
    for (uint64_t id : ids)
        Remove(id);
}

} // namespace atrium

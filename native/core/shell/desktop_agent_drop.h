// OLE IDropTarget implementation for desktop agent drop-bar overlay windows.
#pragma once

#include "shell/desktop_agent_ipc.h"

#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

struct IAgentDropHost {
    IAgentDropHost() = default;
    IAgentDropHost(const IAgentDropHost&) = delete;
    IAgentDropHost& operator=(const IAgentDropHost&) = delete;
    virtual ~IAgentDropHost() = default;
    virtual AgentDragOverResult QueryDragOver(POINT pt) = 0;
    virtual void NotifyDragLeave(uint64_t barId) = 0;
    virtual bool NotifyDropFiles(uint64_t barId, const std::wstring& key, const std::vector<std::wstring>& paths) = 0;
};

struct AgentDropTarget : winrt::implements<AgentDropTarget, IDropTarget> {
    IAgentDropHost* app = nullptr;
    uint64_t barId = 0;
    bool hasFiles = false;
    bool inDrag = false;
    std::wstring lastKey;

    AgentDropTarget(IAgentDropHost* owner, uint64_t id) : app(owner), barId(id) {}
    void ResetDrag() { hasFiles = false, inDrag = false, lastKey.clear(); }

    void EvalEffect(POINTL pt, DWORD* effect)
    {
        const AgentDragOverResult res = (hasFiles && app) ? app->QueryDragOver(POINT{pt.x, pt.y}) : AgentDragOverResult{};
        lastKey = res.key;
        *effect = (hasFiles && res.allow && !res.key.empty()) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }

    HRESULT __stdcall DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        if (!effect)
            return E_INVALIDARG;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        hasFiles = data && data->QueryGetData(&fe) == S_OK, inDrag = true, EvalEffect(pt, effect);
        return S_OK;
    }

    HRESULT __stdcall DragOver(DWORD, POINTL pt, DWORD* effect) override { return effect ? (EvalEffect(pt, effect), S_OK) : E_INVALIDARG; }

    HRESULT __stdcall DragLeave() override
    {
        ResetDrag();
        if (app)
            app->NotifyDragLeave(barId);
        return S_OK;
    }

    HRESULT __stdcall Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        if (!effect)
            return E_INVALIDARG;
        const bool files = hasFiles;
        ResetDrag();
        if (!app)
            return *effect = DROPEFFECT_NONE, S_OK;
        const AgentDragOverResult res = files ? app->QueryDragOver(POINT{pt.x, pt.y}) : AgentDragOverResult{};
        app->NotifyDragLeave(barId);
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (!files || !res.allow || res.key.empty() || !data || FAILED(data->GetData(&fe, &sm)))
            return *effect = DROPEFFECT_NONE, S_OK;
        std::vector<std::wstring> paths;
        if (auto* drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count && paths.size() < kMaxAgentDropPaths; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(len > 0 && len <= kMaxAgentPathLength ? len + 1 : 0, L'\0');
                if (!path.empty()) {
                    if (const UINT copied = DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())); copied > 0 && copied <= kMaxAgentPathLength)
                        path.resize(copied), paths.push_back(std::move(path));
                }
            }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
        if (paths.empty())
            return *effect = DROPEFFECT_NONE, S_OK;
        const bool accepted = app->NotifyDropFiles(barId, res.key, paths);
        return *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE, S_OK;
    }
};

} // namespace atrium

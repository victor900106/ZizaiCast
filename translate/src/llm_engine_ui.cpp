// pm/llm_translate.h: the settings page side of the local LLM engine.
#include <windows.h>
#include <shlobj.h>

#include "llm_engine.h"
#include "pm/llm_translate.h"
#include "text_util.h"

namespace pm::translate::local_llm {

namespace {

const llm::ModelInfo* pick(const std::string& id) { return id.empty() ? &llm::activeModel() : llm::findModel(id); }

Result classify(const std::wstring& e) {
    auto has = [&](const wchar_t* s) { return e.find(s) != std::wstring::npos; };
    if (has(L"cancelled")) return Result::Cancelled;
    if (has(L"SHA-256")) return Result::Verify;
    if (has(L"tar")) return Result::Unpack;
    if (has(L"cannot write") || has(L"cannot move")) return Result::Disk;
    return Result::Network;
}

}  // namespace

std::vector<Model> models() {
    std::vector<Model> v;
    const auto& all = llm::models();
    const std::string def = llm::defaultModel().id;
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& m = all[i];
        Model o;
        o.id = m.id;
        o.name = fromUtf8(m.name);
        o.license = fromUtf8(m.license);
        o.fileBytes = m.size;
        o.ramMB = m.ramMB;
        o.recommended = def == m.id;
        o.installed = llm::installed(m);
        v.push_back(std::move(o));
    }
    return v;
}

Settings loadSettings() {
    Settings s;
    s.enabled = llm::enabled();
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"llm", L"model", L"", buf, 256, llm::settingsPath().c_str());
    s.model = toUtf8(buf);
    s.gpu = llm::gpuEnabled();
    if (!llm::findModel(s.model)) s.model.clear();
    return s;
}

bool saveSettings(const Settings& s, std::wstring* err) {
    const std::wstring path = llm::settingsPath();
    const std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const bool ok = WritePrivateProfileStringW(L"llm", L"enabled", s.enabled ? L"1" : L"0", path.c_str()) &&
                    WritePrivateProfileStringW(L"llm", L"model", fromUtf8(s.model).c_str(), path.c_str()) &&
                    WritePrivateProfileStringW(L"llm", L"gpu", s.gpu ? L"1" : L"0", path.c_str());
    if (!ok && err) *err = L"cannot write " + path + L" (error " + std::to_wstring(GetLastError()) + L")";
    llm::releaseModel();  // loaded again with the new settings on the next request
    return ok;
}

Status status() {
    const llm::ModelInfo& m = llm::activeModel();
    Status st;
    st.model = m.id;
    st.installed = llm::installed(m);
    st.enabled = llm::enabled();
    st.memoryOk = llm::memoryOk(m);
    st.missingBytes = llm::missingBytes(m);
    st.partialBytes = llm::partialBytes(m);
    st.gpuPossible = llm::gpuPossible();
    st.gpuInstalled = llm::gpuInstalled();
    st.discreteGpu = llm::discreteGpu();
    st.gpuWillDownload = llm::gpuWanted() && !st.gpuInstalled;
    st.device = llm::LlmEngine().device();
    return st;
}

std::wstring describeDownload() { return llm::describeDownload(llm::activeModel()); }

const char* resultId(Result r) {
    switch (r) {
    case Result::Ok: return "ok";
    case Result::Cancelled: return "cancelled";
    case Result::Network: return "network";
    case Result::Verify: return "verify";
    case Result::Disk: return "disk";
    case Result::Unpack: return "unpack";
    case Result::UnknownModel: return "unknown-model";
    }
    return "?";
}

Result download(const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::wstring* detail,
                const std::string& model) {
    const llm::ModelInfo* m = pick(model);
    if (!m) return Result::UnknownModel;
    std::wstring e;
    if (llm::download(*m, progress, cancel, &e)) return Result::Ok;
    if (detail) *detail = e;
    return classify(e);
}

Result download(const std::function<void(const DownloadProgress&)>& progress, const std::atomic<bool>* cancel,
                std::wstring* detail, const std::string& model) {
    const llm::ModelInfo* m = pick(model);
    if (!m) return Result::UnknownModel;
    std::wstring e;
    auto relay = [&](const dl::Progress& p) {
        if (!progress) return;
        DownloadProgress d;
        d.fraction = p.fraction();
        d.doneBytes = p.done;
        d.totalBytes = p.total;
        d.bytesPerSec = p.bytesPerSec;
        d.secondsLeft = p.secondsLeft;
        d.connections = p.connections;
        d.verifying = p.verifying;
        d.pending = p.pending;
        progress(d);
    };
    if (llm::download(*m, std::function<void(const dl::Progress&)>(relay), cancel, &e)) return Result::Ok;
    if (detail) *detail = e;
    return classify(e);
}

bool remove(const std::string& model) {
    const llm::ModelInfo* m = pick(model);
    if (!m) return false;
    llm::releaseModel();
    return llm::remove(*m);
}

void release() { llm::releaseModel(); }

void warmUp() { llm::warmUp(); }

}  // namespace pm::translate::local_llm

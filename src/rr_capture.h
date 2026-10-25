#pragma once
#include "rr_capture_model.h"
#include "rr_live_gpu.h"
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
namespace rrcapture {
// D3D12 side of an RR input capture: layout, readback buffers, copies and the file writer.
using Microsoft::WRL::ComPtr;
using Capture = Frame<ComPtr<ID3D12Resource>>;
using Session = Sequence<Capture>;

// One per Input; resource == nullptr means not sent this frame. Every resource is in
// D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE when Record runs; only subresource 0 is copied.
struct Source {
    ID3D12Resource *resource{};
    UINT x{}, y{};
};
using Sources = std::array<Source, InputCount>;

// Lays out a w x h box of every present source in one buffer. Leaves out untouched on failure.
inline bool Plan(ID3D12Device *device, UINT w, UINT h, const Sources &sources, Capture &out) {
    if (!device || !w || !h)
        return false;
    std::array<Region, InputCount> regions{};
    const UINT64 align = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    UINT64 offset = 0;
    for (unsigned i = 0; i < InputCount; ++i) {
        const Source &s = sources[i];
        if (!s.resource)
            continue;
        for (unsigned j = 0; j < i; ++j)
            if (sources[j].resource == s.resource)
                return false; // Its barriers would conflict.
        const auto d = s.resource->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 ||
            (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || UINT64(s.x) + w > d.Width ||
            UINT64(s.y) + h > d.Height)
            return false;
        auto copy = rrlive::TextureDesc(w, h, d.Format);
        copy.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows{};
        UINT64 row{}, total{};
        device->GetCopyableFootprints(&copy, 0, 1, offset, &fp, &rows, &row, &total);
        // A planar format's footprint names its plane format, which Record could not rebuild.
        if (rows != h || row % w || total == UINT64_MAX ||
            total > UINT64_MAX - (align - 1) - offset || fp.Footprint.Format != d.Format)
            return false;
        Region &r = regions[i];
        r.present = true;
        r.format = d.Format;
        r.width = w;
        r.height = h;
        r.bytes_per_pixel = unsigned(row / w);
        r.offset = fp.Offset;
        r.pitch = fp.Footprint.RowPitch;
        r.source_x = s.x;
        r.source_y = s.y;
        r.source_width = unsigned(d.Width);
        r.source_height = d.Height;
        offset = (offset + total + align - 1) / align * align;
    }
    if (!offset)
        return false;
    out.width = w;
    out.height = h;
    out.bytes = offset;
    out.regions = regions;
    return true;
}
// Whether bytes fit the DXGI budget of the segment READBACK heaps use. No ledger applies.
inline bool Admit(ID3D12Device *device, UINT64 bytes) {
    rrlive::Budget budget;
    budget.limit = UINT64_MAX;
    return budget.AdmitAllocation(device, bytes, D3D12_HEAP_TYPE_READBACK);
}
inline ComPtr<ID3D12Resource> Allocate(ID3D12Device *device, UINT64 bytes) {
    ComPtr<ID3D12Resource> r;
    if (!device || !bytes)
        return r;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    const auto d = rrprivate::Buffer(bytes);
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&r))))
        r.Reset();
    return r;
}
// frame comes from Plan with the same sources; frame.buffer holds at least frame.bytes.
inline void Record(ID3D12GraphicsCommandList *cl, const Capture &frame, const Sources &sources) {
    if (!cl || !frame.buffer)
        return;
    std::array<D3D12_RESOURCE_BARRIER, InputCount> barriers{};
    UINT n = 0;
    // Subresource 0 is what RR reads; other mips or slices may be in any state.
    for (unsigned i = 0; i < InputCount; ++i)
        if (frame.regions[i].present && sources[i].resource) {
            auto &b = barriers[n++];
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {sources[i].resource, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE};
        }
    if (!n)
        return;
    cl->ResourceBarrier(n, barriers.data());
    for (unsigned i = 0; i < InputCount; ++i) {
        const Region &r = frame.regions[i];
        if (!r.present || !sources[i].resource)
            continue;
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = frame.buffer.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = r.offset;
        to.PlacedFootprint.Footprint = {DXGI_FORMAT(r.format), r.width, r.height, 1, UINT(r.pitch)};
        from.pResource = sources[i].resource;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        const UINT x = r.source_x, y = r.source_y;
        const D3D12_BOX box{x, y, 0, x + r.width, y + r.height, 1};
        cl->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }
    for (UINT i = 0; i < n; ++i)
        std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    cl->ResourceBarrier(n, barriers.data());
}

struct Outcome {
    std::mutex mutex;
    bool done{}, ok{};
    std::string text;
};
inline std::string Utf8(const std::filesystem::path &path) {
    const std::wstring &w = path.native();
    const int n = w.empty() ? 0
                            : WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0,
                                                  nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
// <game folder>\mw4-rr-captures\<stamp>, or <stamp>-2, -3, ... when taken. Creates it.
inline std::filesystem::path Folder(const char *stamp) {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (!n || n >= exe.size())
        throw std::runtime_error("cannot find the game folder");
    exe.resize(n);
    const auto root = std::filesystem::path(exe).parent_path() / L"mw4-rr-captures";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec)
        throw std::runtime_error("cannot create " + Utf8(root));
    for (unsigned k = 1; k <= 99; ++k) {
        const auto folder =
            root / (k == 1 ? std::string(stamp) : stamp + ("-" + std::to_string(k)));
        if (std::filesystem::create_directory(folder, ec))
            return folder;
        if (ec && !std::filesystem::exists(folder, ec))
            throw std::runtime_error("cannot create " + Utf8(folder));
    }
    throw std::runtime_error("no free folder name in " + Utf8(root));
}
// Writes frame index's present regions as packed files; adds their sizes to written.
inline void WriteImages(const std::filesystem::path &folder, unsigned index, const Capture &f,
                        std::uint64_t &written) {
    const std::string at = "frame " + std::to_string(index);
    if (!f.buffer)
        throw std::runtime_error(at + " has no buffer");
    if (f.buffer->GetDesc().Width < f.bytes)
        throw std::runtime_error(at + " buffer is smaller than its layout");
    for (unsigned i = 0; i < InputCount; ++i)
        if (f.regions[i].present && !Fits(f.regions[i], f.bytes))
            throw std::runtime_error(at + " " + names[i] + " does not fit its buffer");
    const D3D12_RANGE read{0, SIZE_T(f.bytes)};
    void *data{};
    if (FAILED(f.buffer->Map(0, &read, &data)) || !data)
        throw std::runtime_error("cannot map " + at);
    struct Mapping {
        ID3D12Resource *resource;
        ~Mapping() {
            const D3D12_RANGE none{};
            resource->Unmap(0, &none);
        }
    } mapping{f.buffer.Get()};
    const auto *base = static_cast<const unsigned char *>(data);
    for (unsigned i = 0; i < InputCount; ++i) {
        const Region &r = f.regions[i];
        if (!r.present)
            continue;
        const auto path = folder / FileName(index, i);
        std::ofstream out(path, std::ios::binary);
        PackRows(base, r, [&](const unsigned char *row, std::uint64_t bytes) {
            out.write(reinterpret_cast<const char *>(row), std::streamsize(bytes));
        });
        out.close();
        if (!out)
            throw std::runtime_error("cannot write " + Utf8(path));
        written += std::uint64_t(r.width) * r.bytes_per_pixel * r.height;
    }
}
// Writes every frame, then manifest.json; returns the folder.
inline std::filesystem::path Store(const std::vector<Capture> &frames, Header header,
                                   std::uint64_t &written) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char text[64];
    std::snprintf(text, sizeof text, "%04hu-%02hu-%02huT%02hu:%02hu:%02hu", t.wYear, t.wMonth,
                  t.wDay, t.wHour, t.wMinute, t.wSecond);
    header.created = text;
    std::snprintf(text, sizeof text, "%04hu%02hu%02hu-%02hu%02hu%02hu", t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond);
    const auto folder = Folder(text);
    for (size_t i = 0; i < frames.size(); ++i)
        WriteImages(folder, unsigned(i), frames[i], written);
    const auto path = folder / L"manifest.json";
    const std::string manifest = Manifest(header, frames);
    std::ofstream out(path, std::ios::binary);
    out.write(manifest.data(), std::streamsize(manifest.size()));
    out.close();
    if (!out)
        throw std::runtime_error("cannot write " + Utf8(path));
    written += manifest.size();
    return folder;
}
// Catches every error. Releases the frames' buffers, logs, then marks outcome done.
inline void Save(std::vector<Capture> &frames, Header header, void (*log)(const char *),
                 Outcome &outcome) {
    bool ok = false;
    std::string text, line;
    char failure[1024]{};
    try {
        std::uint64_t written = 0;
        text = Utf8(Store(frames, std::move(header), written));
        line = "RR capture: saved " + std::to_string(frames.size()) + " frames (" +
               std::to_string((written + (1u << 19)) >> 20) + " MB) to " + text;
        ok = true;
    } catch (const std::exception &e) {
        std::snprintf(failure, sizeof failure, "%s", e.what());
    } catch (...) {
        std::snprintf(failure, sizeof failure, "unexpected error");
    }
    frames.clear();
    try {
        if (!ok) {
            text = failure;
            line = "RR capture failed: " + text;
        }
        if (log)
            log(line.c_str());
    } catch (...) {
    }
    std::lock_guard lock(outcome.mutex);
    outcome.ok = ok;
    outcome.text.swap(text);
    outcome.done = true;
}
struct Job {
    std::vector<Capture> frames;
    Header header;
    void (*log)(const char *);
    std::shared_ptr<Outcome> outcome;
    HMODULE module{};
};
inline DWORD WINAPI Writer(void *p) {
    const HMODULE module = static_cast<Job *>(p)->module;
    try {
        std::unique_ptr<Job> job(static_cast<Job *>(p));
        Save(job->frames, std::move(job->header), job->log, *job->outcome);
    } catch (...) {
    }
    FreeLibraryAndExitThread(module, 0);
}
// Writes frames on a background thread (on this one if it cannot start). The outcome is done
// when the files are complete or have failed; log may be null.
inline std::shared_ptr<Outcome> Write(std::vector<Capture> frames, Header header,
                                      void (*log)(const char *)) {
    auto outcome = std::make_shared<Outcome>();
    std::unique_ptr<Job> job(new (std::nothrow)
                                 Job{std::move(frames), std::move(header), log, outcome});
    // The thread holds a reference on this DLL so unloading cannot pull its code out from under it.
    if (job && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                  reinterpret_cast<LPCWSTR>(&Writer), &job->module)) {
        Job *owned = job.release();
        if (const HANDLE thread = CreateThread(nullptr, 0, &Writer, owned, 0, nullptr)) {
            CloseHandle(thread);
            return outcome;
        }
        job.reset(owned);
        FreeLibrary(job->module);
    }
    if (job) {
        frames = std::move(job->frames);
        header = std::move(job->header);
    }
    Save(frames, std::move(header), log, *outcome);
    return outcome;
}
} // namespace rrcapture

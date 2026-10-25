#include "../src/rr_oit_signature.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
static void Check(bool value, const char *label) {
    if (!value) {
        printf("FAIL: %s\n", label);
        exit(1);
    }
}

// Fake COM objects, laid out as the D3D12 ABI lays them out: a vtable pointer
// first. The tests call them through the SDK interfaces, so the compiler takes
// each vtable slot from d3d12.h, independently of the indices the hooks read.
struct Fake {
    void **vtable{};
    const IID *iid{};
    ULONG refs = 1;
    std::vector<unsigned char> data; // private data stored under SignatureMarkerId
    bool stored{}, throw_on_set{}, fail_on_set{};
    UINT sets{};
};
static void Trap() {
    Check(false, "only the vtable slots under test are called");
}
static HRESULT STDMETHODCALLTYPE Query(Fake *self, REFIID iid, void **out) {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != *self->iid)
        return E_NOINTERFACE;
    ++self->refs;
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE AddRef(Fake *self) {
    return ++self->refs;
}
static ULONG STDMETHODCALLTYPE Release(Fake *self) {
    return --self->refs;
}
static HRESULT STDMETHODCALLTYPE GetData(Fake *self, REFGUID id, UINT *size, void *data) {
    Check(id == rrsig::SignatureMarkerId, "only the signature marker is read");
    if (!size)
        return E_INVALIDARG;
    const UINT have = UINT(self->data.size());
    if (!self->stored) {
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    if (data && *size < have) {
        *size = have;
        return DXGI_ERROR_MORE_DATA;
    }
    if (data)
        memcpy(data, self->data.data(), have);
    *size = have;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE SetData(Fake *self, REFGUID id, UINT size, const void *data) {
    Check(id == rrsig::SignatureMarkerId, "only the signature marker is written");
    ++self->sets;
    SetLastError(62);
    if (self->throw_on_set)
        throw 1;
    if (size && self->fail_on_set)
        return E_OUTOFMEMORY;
    self->stored = size && data;
    self->data.assign(static_cast<const unsigned char *>(data),
                      static_cast<const unsigned char *>(data) + (self->stored ? size : 0));
    return S_OK;
}

static void *device_vtable[64], *signature_vtable[16], *list_vtable[128];
static Fake device, list;
// Read through volatile pointers so calls stay true vtable calls into the hooks.
static Fake *volatile device_object = &device;
static Fake *volatile list_object = &list;
static void Bind(Fake &object, void **vtable, unsigned slots, const IID &iid) {
    for (unsigned i = 0; i < slots; ++i)
        vtable[i] = reinterpret_cast<void *>(&Trap);
    vtable[0] = reinterpret_cast<void *>(&Query);
    vtable[1] = reinterpret_cast<void *>(&AddRef);
    vtable[2] = reinterpret_cast<void *>(&Release);
    object.vtable = vtable;
    object.iid = &iid;
}
static void BindSignature(Fake &object) {
    Bind(object, signature_vtable, 16, __uuidof(ID3D12CommandSignature));
    signature_vtable[3] = reinterpret_cast<void *>(&GetData);
    signature_vtable[4] = reinterpret_cast<void *>(&SetData);
}

// ID3D12Device::CreateCommandSignature stand-in; the hook goes on its prologue.
static Fake *next_signature{};
static HRESULT create_result = S_OK;
static const D3D12_COMMAND_SIGNATURE_DESC *created_desc{};
static ID3D12RootSignature *created_root{};
static unsigned creates{};
__declspec(noinline) static HRESULT STDMETHODCALLTYPE
CreateSignature(Fake *self, const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root,
                REFIID iid, void **out) {
    Check(self == &device && iid == __uuidof(ID3D12CommandSignature), "creation identity");
    ++creates;
    created_desc = desc;
    created_root = root;
    SetLastError(61);
    if (SUCCEEDED(create_result) && out) {
        ++next_signature->refs;
        *out = next_signature;
    }
    return create_result;
}

// ID3D12GraphicsCommandList::ExecuteIndirect stand-ins. OuterExecuteIndirect
// forwards into the list's ExecuteIndirect, as a hooked runtime or debug layer
// forwards into the next layer's command list.
struct Native {
    ID3D12GraphicsCommandList *cl;
    ID3D12CommandSignature *signature;
    UINT max;
    ID3D12Resource *args;
    UINT64 args_offset;
    ID3D12Resource *count;
    UINT64 count_offset;
};
static std::mutex record; // guards events and natives: two threads record at once
static std::string events; // L: outer layer, N: native ExecuteIndirect, O: observer
static std::vector<Native> natives;
static void *outer_vtable[128];
static Fake outer;
static Fake *volatile outer_object = &outer;
static ID3D12GraphicsCommandList *List() {
    return reinterpret_cast<ID3D12GraphicsCommandList *>(list_object);
}
static ID3D12GraphicsCommandList *Outer() {
    return reinterpret_cast<ID3D12GraphicsCommandList *>(outer_object);
}
__declspec(noinline) static void STDMETHODCALLTYPE
ExecuteIndirect(ID3D12GraphicsCommandList *cl, ID3D12CommandSignature *signature, UINT max,
                ID3D12Resource *args, UINT64 args_offset, ID3D12Resource *count,
                UINT64 count_offset) {
    {
        std::lock_guard lock(record);
        events += 'N';
        natives.push_back({cl, signature, max, args, args_offset, count, count_offset});
    }
    SetLastError(73);
}
__declspec(noinline) static void STDMETHODCALLTYPE
OuterExecuteIndirect(ID3D12GraphicsCommandList *cl, ID3D12CommandSignature *signature, UINT max,
                     ID3D12Resource *args, UINT64 args_offset, ID3D12Resource *count,
                     UINT64 count_offset) {
    Check(cl == Outer(), "outer layer identity");
    {
        std::lock_guard lock(record);
        events += 'L';
    }
    List()->ExecuteIndirect(signature, max, args, args_offset, count, count_offset);
}
static auto *const indirect_signature = reinterpret_cast<ID3D12CommandSignature *>(0x5100);
static auto *const indirect_args = reinterpret_cast<ID3D12Resource *>(0x5200);
static auto *const indirect_count = reinterpret_cast<ID3D12Resource *>(0x5300);
static void Execute(UINT max, UINT64 args_offset, UINT64 count_offset,
                    ID3D12GraphicsCommandList *game = List()) {
    events.clear();
    natives.clear();
    game->ExecuteIndirect(indirect_signature, max, indirect_args, args_offset, indirect_count,
                          count_offset);
}
static bool Same(const Native &n, UINT max, UINT64 args_offset, UINT64 count_offset) {
    return n.cl == List() && n.signature == indirect_signature && n.max == max &&
           n.args == indirect_args && n.args_offset == args_offset && n.count == indirect_count &&
           n.count_offset == count_offset;
}
static bool Same(const rrcompute::IndirectCall &c, const Native &n) {
    return c.cl == n.cl && c.signature == n.signature && c.max == n.max && c.args == n.args &&
           c.args_offset == n.args_offset && c.count == n.count &&
           c.count_offset == n.count_offset && c.replay;
}

namespace rrcompute {
struct TrackerTestAccess {
    enum class Mode { Record, Replay, Misuse, NoTrampoline, WrongLayer, Throw, ThrowTogether };
    static inline Tracker *tracker{};
    static inline Mode mode{};
    static inline std::vector<IndirectCall> seen;
    static inline std::vector<bool> replays; // what each replay attempt returned
    static inline bool lock_free{}, updated{}, under_internal{}, replay_untracked{}, inside{};
    static inline std::atomic<unsigned> logs{}, arrivals{};
    static void Log(const char *) {
        ++logs;
        SetLastError(98);
    }
    static Tracker::State *Find(ID3D12GraphicsCommandList *cl) {
        auto i = tracker->states.find(cl);
        return i == tracker->states.end() ? nullptr : &i->second;
    }
    static void Observer(const IndirectCall &call) {
        Check(!inside, "the replay never re-enters the observer");
        {
            std::lock_guard lock(record);
            events += 'O';
            seen.push_back(call);
        }
        SetLastError(99);
        if (mode == Mode::ThrowTogether) {
            // Hold both threads inside the observer, then throw from both.
            ++arrivals;
            const auto until = GetTickCount64() + 5000;
            while (arrivals < 2 && GetTickCount64() < until)
                std::this_thread::yield();
            throw 1;
        }
        // Probe from another thread: try_lock on a mutex this thread owns is undefined.
        std::thread([] {
            lock_free = tracker->mutex.try_lock();
            if (lock_free)
                tracker->mutex.unlock();
        }).join();
        auto *state = Find(call.cl);
        updated = state && state->uncertain;
        under_internal = Tracker::internal;
        inside = true;
        switch (mode) {
        case Mode::Replay:
            // The replay must bypass the detour by itself, so neither Update nor
            // this observer may run for it.
            if (state)
                state->uncertain = false;
            replays.push_back(call.replay(call));
            replay_untracked = !state || !state->uncertain;
            break;
        case Mode::Misuse: {
            auto changed = call;
            changed.args_offset += 4;
            replays.push_back(changed.replay(changed));
            replays.push_back(call.replay(call));
            replays.push_back(call.replay(call));
            break;
        }
        case Mode::NoTrampoline: {
            auto &slot = tracker->hooks[Tracker::Indirect][0].original;
            void *original = slot.exchange(nullptr);
            replays.push_back(call.replay(call));
            slot = original;
            break;
        }
        case Mode::WrongLayer:
            // The call came through layer 1; layer 0's trampoline must refuse it.
            replays.push_back(Tracker::Replay<0>(call));
            replays.push_back(call.replay(call));
            break;
        default:
            break;
        }
        inside = false;
        if (mode == Mode::Throw)
            throw 1;
    }
    static void OtherObserver(const IndirectCall &) {
        Check(false, "a refused observer is never called");
    }
    static void OtherLog(const char *) {
        Check(false, "a refused logger is never called");
    }
    static bool Healthy(const Tracker &t) {
        return t.healthy && t.target_healthy && t.sss_healthy && t.outputs_healthy &&
               t.stencil_healthy && t.write_observers_healthy;
    }
    // Whether t still hands a forwarded call to its observer.
    static bool Observes(Tracker &t) {
        tracker = &t;
        mode = Mode::Record;
        seen.clear();
        t.AfterIndirect({List(), indirect_signature, 1, indirect_args, 0, indirect_count, 0,
                         nullptr});
        tracker = nullptr;
        return seen.size() == 1;
    }
    static void Registration() {
        auto *cl = List();
        for (bool null_observer : {true, false}) {
            Tracker t;
            Check(!(null_observer ? t.SetAfterIndirect(nullptr, &Log)
                                  : t.SetAfterIndirect(&Observer, nullptr)) &&
                      !t.SetAfterIndirect(&Observer, &Log) && !Observes(t),
                  "an observer registers only with a logger, and a refusal is final");
        }
        for (bool other_observer : {true, false}) {
            Tracker t;
            Check(t.SetAfterIndirect(&Observer, &Log) && t.SetAfterIndirect(&Observer, &Log) &&
                      Observes(t),
                  "the same observer registers before Attach, idempotently");
            Check(!(other_observer ? t.SetAfterIndirect(&OtherObserver, &Log)
                                   : t.SetAfterIndirect(&Observer, &OtherLog)) &&
                      !Observes(t),
                  "changing the observer or its logger is refused and turns it off");
            Check(!t.SetAfterIndirect(&Observer, &Log) && !Observes(t),
                  "a refused observer stays off");
            Check(Healthy(t), "a refused observer leaves the tracker healthy");
        }
        {
            Tracker t;
            t.states.try_emplace(cl);
            Check(!t.SetAfterIndirect(&Observer, &Log) && !Observes(t),
                  "an observer registered after Attach is refused");
        }
        {
            Tracker t;
            Check(t.SetAfterIndirect(&Observer, &Log), "observer registers");
            tracker = &t;
            mode = Mode::Throw;
            t.AfterIndirect({cl, indirect_signature, 1, indirect_args, 0, indirect_count, 0,
                             nullptr});
            Check(logs == 1, "the exception is logged");
            Check(t.states.empty() && !t.SetAfterIndirect(&Observer, &Log) && !Observes(t),
                  "an observer disabled by an exception stays off, even before Attach");
            logs = 0;
        }
    }
    static void Run() {
        Tracker t;
        tracker = &t;
        Tracker::active = &t;
        Check(t.SetAfterIndirect(&Observer, &Log), "observer registers before Attach");
        Check(t.Hook<Tracker::Indirect>(list_vtable[59]), "ExecuteIndirect detour installs");
        auto *cl = List();
        t.states.try_emplace(cl);
        Check(t.SetAfterIndirect(&Observer, &Log), "re-registering the same observer keeps it");

        mode = Mode::Record;
        seen.clear();
        Find(cl)->uncertain = false;
        SetLastError(5);
        Execute(3, 16, 32);
        Check(events == "NO", "the observer runs once, after the forwarded call");
        Check(natives.size() == 1 && Same(natives[0], 3, 16, 32),
              "the native call is forwarded unchanged");
        Check(seen.size() == 1 && Same(seen[0], natives[0]),
              "the observer sees the forwarded arguments");
        Check(updated && lock_free, "Update ran and released the tracker lock first");
        Check(under_internal && !Tracker::internal,
              "the observer runs under Tracker::Internal, and only the observer");
        Check(GetLastError() == 73, "the native call's last error survives the observer");
        const auto unreplayed = seen[0];
        Check(!unreplayed.replay(unreplayed) && natives.size() == 1,
              "a call is not replayed after its observer returned");

        seen.clear();
        Find(cl)->uncertain = false;
        {
            Tracker::Internal internal;
            Execute(4, 0, 0);
        }
        Check(events == "N" && seen.empty() && !Find(cl)->uncertain,
              "add-on recording is forwarded but neither tracked nor observed");

        mode = Mode::Replay;
        replays.clear();
        Execute(5, 48, 64);
        Check(events == "NON" && seen.size() == 1 && replays == std::vector<bool>{true},
              "the replay is forwarded once and never re-enters the observer");
        Check(natives.size() == 2 && Same(natives[0], 5, 48, 64) && Same(natives[1], 5, 48, 64),
              "the replay repeats the native arguments exactly");
        Check(replay_untracked, "the replay never reaches Update");
        const auto replayed = seen[0];
        Check(!replayed.replay(replayed) && natives.size() == 2,
              "a replayed call is not replayed again after its observer returned");

        mode = Mode::Misuse;
        seen.clear();
        replays.clear();
        Execute(5, 48, 64);
        Check(replays == std::vector<bool>{false, true, false} && events == "NON" &&
                  Same(natives[1], 5, 48, 64),
              "only the unmodified call is replayed, and only once");
        mode = Mode::NoTrampoline;
        seen.clear();
        replays.clear();
        Execute(5, 48, 64);
        Check(replays == std::vector<bool>{false} && events == "NO",
              "no replay without a trampoline");
        Check(logs == 0, "nothing logged while the observer works");

        seen.clear();
        mode = Mode::Throw;
        SetLastError(5);
        try {
            Execute(6, 0, 8);
        } catch (...) {
            Check(false, "an observer exception never reaches the game");
        }
        Check(events == "NO" && GetLastError() == 73,
              "a throwing observer still follows the forwarded call and keeps its error");
        Check(logs == 1, "the exception is logged once");
        Check(Healthy(t), "an observer exception leaves the tracker healthy");

        seen.clear();
        mode = Mode::Record;
        Find(cl)->uncertain = false;
        Execute(7, 0, 0);
        Check(events == "N" && seen.empty() && logs == 1,
              "the disabled observer is not called again");
        Check(natives.size() == 1 && Same(natives[0], 7, 0, 0) && Find(cl)->uncertain,
              "the tracker keeps forwarding and tracking after the exception");
        Check(!t.SetAfterIndirect(&Observer, &Log), "a disabled observer cannot come back");

        t.RemoveForTest();
        tracker = nullptr;
        logs = 0;
    }
    // Two hooked layers, the outer forwarding into the inner. Only the game's
    // own call, at the outermost layer, is observed.
    static void Stacked() {
        Tracker t;
        tracker = &t;
        Tracker::active = &t;
        Check(t.SetAfterIndirect(&Observer, &Log), "observer registers before Attach");
        Check(t.Hook<Tracker::Indirect>(list_vtable[59]) &&
                  t.Hook<Tracker::Indirect>(outer_vtable[59]),
              "both layers are hooked");
        t.states.try_emplace(Outer());

        mode = Mode::Record;
        seen.clear();
        Execute(3, 16, 32, Outer());
        Check(events == "LNO" && seen.size() == 1,
              "one game call through two hooked layers is observed once, after both");
        Check(natives.size() == 1 && Same(natives[0], 3, 16, 32),
              "both layers forward the call unchanged");
        Check(seen[0].cl == Outer() && seen[0].args_offset == 16 && seen[0].count_offset == 32,
              "the observer sees the outermost layer's call");

        mode = Mode::Replay;
        seen.clear();
        replays.clear();
        Execute(5, 48, 64, Outer());
        Check(events == "LNOLN" && seen.size() == 1 && replays == std::vector<bool>{true},
              "a replay through both layers is never observed");
        Check(natives.size() == 2 && Same(natives[1], 5, 48, 64),
              "a replay through both layers adds exactly one native execute");

        mode = Mode::WrongLayer;
        seen.clear();
        replays.clear();
        Execute(5, 48, 64, Outer());
        Check(replays == std::vector<bool>{false, true} && events == "LNOLN",
              "a call replays only through the layer that forwarded it");

        mode = Mode::Record;
        seen.clear();
        {
            Tracker::Internal internal;
            Execute(4, 0, 0, Outer());
        }
        Check(events == "LN" && seen.empty(), "add-on recording is observed at no layer");
        Check(logs == 0, "nothing logged");
        t.RemoveForTest();
        tracker = nullptr;
    }
    // Both threads throw from inside the observer at the same time.
    static void ConcurrentThrows() {
        Tracker t;
        tracker = &t;
        Tracker::active = &t;
        Check(t.SetAfterIndirect(&Observer, &Log), "observer registers before Attach");
        Check(t.Hook<Tracker::Indirect>(list_vtable[59]), "ExecuteIndirect detour installs");
        t.states.try_emplace(List());
        mode = Mode::ThrowTogether;
        arrivals = 0;
        events.clear();
        natives.clear();
        auto game = [] {
            List()->ExecuteIndirect(indirect_signature, 2, indirect_args, 0, indirect_count, 0);
        };
        std::thread a(game), b(game);
        a.join();
        b.join();
        Check(arrivals == 2 && natives.size() == 2,
              "both threads forwarded and were inside the observer at once");
        Check(logs == 1, "two simultaneous observer exceptions log once");
        mode = Mode::Record;
        seen.clear();
        Execute(3, 0, 0);
        Check(events == "N" && seen.empty() && !t.SetAfterIndirect(&Observer, &Log),
              "the observer stays off after simultaneous exceptions");
        t.RemoveForTest();
        tracker = nullptr;
        logs = 0;
    }
};
} // namespace rrcompute

static void RootConstantWords() {
    rrcompute::Tracker t;
    auto *root = reinterpret_cast<ID3D12RootSignature *>(0x7000);
    rrcompute::Shape shapes[4]{};
    shapes[0].kind = rrcompute::Kind::Table;
    shapes[1].kind = rrcompute::Kind::Constants;
    shapes[1].words = 4;
    shapes[2].kind = rrcompute::Kind::CBV;
    shapes[3].kind = rrcompute::Kind::Constants;
    shapes[3].words = 1;
    t.Define(root, 4, shapes);
    Check(t.RootConstantWords(root, 1) == 4 && t.RootConstantWords(root, 3) == 1,
          "root-constant word counts come from the defined layout");
    Check(t.RootConstantWords(root, 0) == -1 && t.RootConstantWords(root, 2) == -1,
          "a parameter that is not root constants is unknown");
    Check(t.RootConstantWords(root, 4) == -1 && t.RootConstantWords(root, 64) == -1,
          "a parameter past the layout is unknown");
    Check(t.RootConstantWords(reinterpret_cast<ID3D12RootSignature *>(0x7100), 1) == -1 &&
              t.RootConstantWords(nullptr, 1) == -1,
          "an undefined root signature is unknown");
    t.ForgetRoot(root);
    Check(t.RootConstantWords(root, 1) == -1, "a forgotten root signature is unknown");
}

static rrcompute::Tracker layouts; // outlives the CreateCommandSignature hook
static std::vector<std::string> signature_logs;
static void SignatureLog(const char *text) {
    signature_logs.push_back(text);
    SetLastError(97);
}
static auto *const root = reinterpret_cast<ID3D12RootSignature *>(0x7000);
static ID3D12Device *Device() {
    return reinterpret_cast<ID3D12Device *>(device_object);
}
static ID3D12CommandSignature *Create(Fake &object, const D3D12_COMMAND_SIGNATURE_DESC &desc,
                                      ID3D12RootSignature *with) {
    next_signature = &object;
    const auto refs = object.refs;
    ID3D12CommandSignature *out{};
    SetLastError(5);
    const auto hr = Device()->CreateCommandSignature(&desc, with, IID_PPV_ARGS(&out));
    Check(hr == S_OK && out == reinterpret_cast<ID3D12CommandSignature *>(&object),
          "creation result passes through the hook");
    Check(GetLastError() == 61, "creation's last error survives the marker");
    Check(created_desc == &desc && created_root == with, "creation arguments pass unchanged");
    Check(object.refs == refs + 1, "the hook releases every reference it takes");
    return out;
}
static void Flip(std::vector<unsigned char> &bytes, size_t at) {
    bytes[at] = static_cast<unsigned char>(~bytes[at]);
}
static rrsig::SignatureClass Classify(const D3D12_COMMAND_SIGNATURE_DESC &desc,
                                      ID3D12RootSignature *with) {
    Fake object;
    BindSignature(object);
    return rrsig::Read(Create(object, desc, with));
}
static void Signatures() {
    using rrsig::SignatureClass;
    rrcompute::Shape shapes[3]{};
    shapes[0].kind = rrcompute::Kind::Table;
    shapes[1].kind = rrcompute::Kind::Constants;
    shapes[1].words = 4;
    shapes[2].kind = rrcompute::Kind::CBV;
    layouts.Define(root, 3, shapes);
    Bind(device, device_vtable, 64, __uuidof(ID3D12Device));
    device_vtable[41] = reinterpret_cast<void *>(&CreateSignature);

    Check(!rrsig::Attached(), "no signature hook before Attach");
    Check(!rrsig::Attach(nullptr, layouts, &SignatureLog) && !rrsig::Attached(),
          "a null device is refused");
    Check(rrsig::Attach(Device(), layouts, &SignatureLog) && rrsig::Attached(),
          "CreateCommandSignature is hooked at device vtable index 41");
    Check(rrsig::Attach(Device(), layouts, &SignatureLog),
          "attaching the same device again reuses the hook");
    Check(signature_logs.empty(), "nothing logged while attaching");
    {
        rrcompute::Tracker other;
        SetLastError(5);
        Check(!rrsig::Attach(Device(), other, &SignatureLog) && GetLastError() == 5,
              "a second tracker is refused");
        Check(signature_logs.size() == 1 &&
                  signature_logs[0].find("tracker") != std::string::npos,
              "the refused tracker is logged");
    }
    Check(device.refs == 1, "Attach releases the device references it takes");

    D3D12_INDIRECT_ARGUMENT_DESC a[3]{};
    a[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    a[0].Constant = {1, 0, 4};
    a[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    a[1].ConstantBufferView.RootParameterIndex = 2;
    a[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    const D3D12_COMMAND_SIGNATURE_DESC full{52, 3, a, 0};
    Fake object;
    BindSignature(object);
    auto *signature = Create(object, full, root);
    Check(object.sets == 1 && rrsig::Read(signature) == SignatureClass::Replayable,
          "whole root constants and a final draw are replayable");
    rrsig::Marker marker{};
    Check(rrsig::ReadMarker(signature, marker) && marker.cls == SignatureClass::Replayable &&
              marker.stride == 52 && marker.arguments == 3 &&
              marker.draw == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW,
          "the marker keeps stride, argument count and draw kind");
    Check(object.refs == 2, "Read releases every reference it takes");

    a[0].Constant.Num32BitValuesToSet = 3;
    Check(Classify(full, root) == SignatureClass::PartialConstants,
          "a partial root-constant write is marked partial");
    a[0].Constant.Num32BitValuesToSet = 4;
    Check(Classify(full, reinterpret_cast<ID3D12RootSignature *>(0x7100)) ==
              SignatureClass::Unknown,
          "constants of an undefined root signature are unknown");
    Check(Classify(full, nullptr) == SignatureClass::Unknown,
          "constants without a root signature are unknown");
    D3D12_INDIRECT_ARGUMENT_DESC indexed{};
    indexed.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    const D3D12_COMMAND_SIGNATURE_DESC draw_only{20, 1, &indexed, 0};
    Fake plain;
    BindSignature(plain);
    Check(rrsig::Read(Create(plain, draw_only, nullptr)) == SignatureClass::Replayable &&
              rrsig::ReadMarker(reinterpret_cast<ID3D12CommandSignature *>(&plain), marker) &&
              marker.draw == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED && marker.stride == 20,
          "a draw-only signature needs no root signature");
    D3D12_INDIRECT_ARGUMENT_DESC dispatch{};
    dispatch.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    Check(Classify({12, 1, &dispatch, 0}, nullptr) == SignatureClass::Unsupported,
          "a dispatch signature is unsupported");

    Fake unseen;
    BindSignature(unseen);
    auto *unseen_signature = reinterpret_cast<ID3D12CommandSignature *>(&unseen);
    Check(rrsig::Read(unseen_signature) == SignatureClass::Unknown &&
              !rrsig::ReadMarker(unseen_signature, marker) &&
              rrsig::Read(nullptr) == SignatureClass::Unknown,
          "a signature without a marker is unknown");
    // Copies of the replayable marker above that differ in one respect only.
    using Bytes = std::vector<unsigned char>;
    auto plant = [&](void (*change)(Bytes &)) {
        unseen.stored = true;
        unseen.data = object.data;
        change(unseen.data);
        return rrsig::Read(unseen_signature);
    };
    Check(plant([](Bytes &) {}) == SignatureClass::Replayable, "an exact copy reads back");
    Check(plant([](Bytes &b) { Flip(b, offsetof(rrsig::Marker, owner)); }) ==
              SignatureClass::Unknown,
          "a marker another add-on instance wrote is unknown");
    Check(plant([](Bytes &b) { Flip(b, offsetof(rrsig::Marker, version)); }) ==
              SignatureClass::Unknown,
          "a marker of another version is unknown");
    Check(plant([](Bytes &b) { b.resize(offsetof(rrsig::Marker, stride)); }) ==
              SignatureClass::Unknown,
          "a marker of another size is unknown");

    const auto made = creates;
    const auto sets = object.sets;
    next_signature = &object;
    create_result = E_OUTOFMEMORY;
    ID3D12CommandSignature *out{};
    SetLastError(5);
    Check(Device()->CreateCommandSignature(&full, root, IID_PPV_ARGS(&out)) == E_OUTOFMEMORY &&
              !out && GetLastError() == 61,
          "a failed creation passes through untouched");
    create_result = S_FALSE;
    Check(Device()->CreateCommandSignature(&full, root, __uuidof(ID3D12CommandSignature),
                                           nullptr) == S_FALSE,
          "a creation probe without an object passes through");
    Check(creates == made + 2 && object.sets == sets, "nothing is marked without a new object");
    create_result = S_OK;

    object.throw_on_set = true;
    Create(object, full, root);
    Check(object.sets == sets + 1 && rrsig::Read(signature) == SignatureClass::Replayable,
          "an exception while marking stays inside the hook");
    object.throw_on_set = false;
    object.fail_on_set = true;
    Create(object, full, root);
    Check(object.sets == sets + 3 && rrsig::Read(signature) == SignatureClass::Unknown,
          "a marker that cannot be written clears the earlier one");
}

int main() {
    Bind(list, list_vtable, 128, __uuidof(ID3D12GraphicsCommandList));
    list_vtable[59] = reinterpret_cast<void *>(&ExecuteIndirect);
    Bind(outer, outer_vtable, 128, __uuidof(ID3D12GraphicsCommandList));
    outer_vtable[59] = reinterpret_cast<void *>(&OuterExecuteIndirect);
    Check(MH_Initialize() == MH_OK, "MinHook initializes");
    rrcompute::TrackerTestAccess::Registration();
    rrcompute::TrackerTestAccess::Run();
    rrcompute::TrackerTestAccess::Stacked();
    rrcompute::TrackerTestAccess::ConcurrentThrows();
    RootConstantWords();
    Signatures();
    puts("PASS: indirect observer order, arguments, Internal skip, outermost layer only, replay "
         "without re-entry, sticky observer exception containment, root-constant words and "
         "command-signature markers");
}

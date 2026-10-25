#include "../src/rr_oit_signature.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

// ID3D12GraphicsCommandList::ExecuteIndirect stand-in.
struct Native {
    ID3D12GraphicsCommandList *cl;
    ID3D12CommandSignature *signature;
    UINT max;
    ID3D12Resource *args;
    UINT64 args_offset;
    ID3D12Resource *count;
    UINT64 count_offset;
};
static std::string events; // N: native ExecuteIndirect, O: observer
static std::vector<Native> natives;
__declspec(noinline) static void STDMETHODCALLTYPE
ExecuteIndirect(ID3D12GraphicsCommandList *cl, ID3D12CommandSignature *signature, UINT max,
                ID3D12Resource *args, UINT64 args_offset, ID3D12Resource *count,
                UINT64 count_offset) {
    events += 'N';
    natives.push_back({cl, signature, max, args, args_offset, count, count_offset});
    SetLastError(73);
}
static auto *const indirect_signature = reinterpret_cast<ID3D12CommandSignature *>(0x5100);
static auto *const indirect_args = reinterpret_cast<ID3D12Resource *>(0x5200);
static auto *const indirect_count = reinterpret_cast<ID3D12Resource *>(0x5300);
static ID3D12GraphicsCommandList *List() {
    return reinterpret_cast<ID3D12GraphicsCommandList *>(list_object);
}
static void Execute(UINT max, UINT64 args_offset, UINT64 count_offset) {
    events.clear();
    natives.clear();
    List()->ExecuteIndirect(indirect_signature, max, indirect_args, args_offset, indirect_count,
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
    enum class Mode { Record, Replay, Throw };
    static inline Tracker *tracker{};
    static inline Mode mode{};
    static inline std::vector<IndirectCall> seen;
    static inline bool lock_free{}, updated{}, replay_untracked{}, inside{};
    static inline unsigned logs{};
    static void Log(const char *) {
        ++logs;
        SetLastError(98);
    }
    static Tracker::State &Of(ID3D12GraphicsCommandList *cl) {
        return tracker->states.at(cl);
    }
    static void Observer(const IndirectCall &call) {
        Check(!inside, "the replay never re-enters the observer");
        events += 'O';
        seen.push_back(call);
        lock_free = tracker->mutex.try_lock();
        if (lock_free)
            tracker->mutex.unlock();
        updated = Of(call.cl).uncertain;
        SetLastError(99);
        if (mode == Mode::Replay) {
            // No Tracker::Internal here on purpose: the replay must bypass the
            // detour by itself, so neither Update nor this observer may run.
            Of(call.cl).uncertain = false;
            inside = true;
            call.replay(call);
            inside = false;
            replay_untracked = !Of(call.cl).uncertain;
        }
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
    static void Registration() {
        auto *cl = List();
        for (bool other_observer : {true, false}) {
            Tracker t;
            Check(!t.SetAfterIndirect(nullptr, &Log) && !t.SetAfterIndirect(&Observer, nullptr),
                  "an observer registers only with a logger");
            Check(t.SetAfterIndirect(&Observer, &Log) && t.SetAfterIndirect(&Observer, &Log),
                  "the same observer registers before Attach, idempotently");
            Check(!(other_observer ? t.SetAfterIndirect(&OtherObserver, &Log)
                                   : t.SetAfterIndirect(&Observer, &OtherLog)) &&
                      !t.after_indirect.load(),
                  "changing the observer or its logger is refused and turns it off");
            Check(Healthy(t), "a refused observer leaves the tracker healthy");
        }
        {
            Tracker t;
            t.states.try_emplace(cl);
            Check(!t.SetAfterIndirect(&Observer, &Log) && !t.after_indirect.load(),
                  "an observer registered after Attach is refused");
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

        Of(cl).uncertain = false;
        SetLastError(5);
        Execute(3, 16, 32);
        Check(events == "NO", "the observer runs once, after the forwarded call");
        Check(natives.size() == 1 && Same(natives[0], 3, 16, 32),
              "the native call is forwarded unchanged");
        Check(seen.size() == 1 && Same(seen[0], natives[0]),
              "the observer sees the forwarded arguments");
        Check(updated && lock_free, "Update ran and released the tracker lock first");
        Check(GetLastError() == 73, "the native call's last error survives the observer");

        seen.clear();
        Of(cl).uncertain = false;
        {
            Tracker::Internal internal;
            Execute(4, 0, 0);
        }
        Check(events == "N" && seen.empty() && !Of(cl).uncertain,
              "add-on recording is forwarded but neither tracked nor observed");

        mode = Mode::Replay;
        Execute(5, 48, 64);
        Check(events == "NON" && seen.size() == 1,
              "the replay is forwarded once and never re-enters the observer");
        Check(natives.size() == 2 && Same(natives[0], 5, 48, 64) && Same(natives[1], 5, 48, 64),
              "the replay repeats the native arguments exactly");
        Check(replay_untracked, "the replay never reaches Update");
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
        Check(logs == 1 && !t.after_indirect.load(), "the exception turns the observer off, once");
        Check(Healthy(t), "an observer exception leaves the tracker healthy");

        seen.clear();
        mode = Mode::Record;
        Of(cl).uncertain = false;
        Execute(7, 0, 0);
        Check(events == "N" && seen.empty() && logs == 1,
              "the disabled observer is not called again");
        Check(natives.size() == 1 && Same(natives[0], 7, 0, 0) && Of(cl).uncertain,
              "the tracker keeps forwarding and tracking after the exception");
        Check(!t.SetAfterIndirect(&Observer, &Log), "a disabled observer cannot come back");

        t.RemoveForTest();
        tracker = nullptr;
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
    Check(!rrsig::Attach(nullptr, layouts) && !rrsig::Attached(), "a null device is refused");
    Check(rrsig::Attach(Device(), layouts) && rrsig::Attached(),
          "CreateCommandSignature is hooked at device vtable index 41");
    Check(rrsig::Attach(Device(), layouts), "attaching the same device again reuses the hook");
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
    Check(MH_Initialize() == MH_OK, "MinHook initializes");
    rrcompute::TrackerTestAccess::Registration();
    rrcompute::TrackerTestAccess::Run();
    RootConstantWords();
    Signatures();
    puts("PASS: indirect observer order, arguments, Internal skip, replay without re-entry, "
         "observer exception containment, root-constant words and command-signature markers");
}

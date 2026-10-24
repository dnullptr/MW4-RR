// Actual addon callbacks + Inspector/MinHook on fake COM surfaces. No GPU.
#define RR_SPARSE_RETIREMENT_TEST
#include "../src/mw4_rr.cpp"
#include <thread>
#include <functional>
#include <stdexcept>

namespace rrsparsecopy {
struct RetirementTest {
    static void WithLock(Pool &pool, const std::function<void()> &action) {
        std::lock_guard lock(pool.mutex);
        action();
    }
};
} // namespace rrsparsecopy
static void Need(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
static std::vector<D3D12_RESOURCE_BARRIER> forwarded;
static std::atomic<unsigned> callback_count{};
static void Post(ID3D12GraphicsCommandList *cl, const D3D12_RESOURCE_BARRIER &b,
                 rrbuffer::BarrierFn original) {
    ++callback_count;
#ifdef RR_RELEASE_NEGATIVE_CONTROL
    OitSceneTransition(cl, b); // Reproduce the missing private-transition guard.
#else
    OwnedTransition(cl, b, original);
#endif
}
struct FakeList {
    void **v;
    std::array<void *, 81> table{};
    static inline volatile unsigned unused[81]{};
    template <size_t N> static void STDMETHODCALLTYPE Stub(FakeList *) {
        unused[N] = unused[N] + 1;
    }
    static HRESULT STDMETHODCALLTYPE QI(FakeList *self, REFIID id, void **out) {
        *out = nullptr;
        if (id == fg::NativeInterfaceId || id == fg::ReShadeUnwrappedId)
            return E_NOINTERFACE;
        *out = self;
        return S_OK;
    }
    static ULONG STDMETHODCALLTYPE Ref(FakeList *) {
        return 2;
    }
    static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE Type(FakeList *) {
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    __declspec(noinline) static void STDMETHODCALLTYPE Barrier(FakeList *, UINT count,
                                                               const D3D12_RESOURCE_BARRIER *b) {
        for (UINT i = 0; i < count; ++i)
            forwarded.push_back(b[i]);
    }
    template <size_t... N> FakeList(std::index_sequence<N...>) : v(table.data()) {
        table = {reinterpret_cast<void *>(&Stub<N>)...};
        table[0] = reinterpret_cast<void *>(&QI);
        table[1] = table[2] = reinterpret_cast<void *>(&Ref);
        table[8] = reinterpret_cast<void *>(&Type);
        table[26] = reinterpret_cast<void *>(&Barrier);
    }
    FakeList() : FakeList(std::make_index_sequence<81>{}) {}
    ID3D12GraphicsCommandList *Get() {
        return reinterpret_cast<ID3D12GraphicsCommandList *>(this);
    }
};
struct FakeQueue {
    void **v;
    std::array<void *, 19> table{};
    static HRESULT STDMETHODCALLTYPE QI(FakeQueue *self, REFIID id, void **out) {
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(ID3D12CommandQueue))
            return E_NOINTERFACE;
        *out = self;
        return S_OK;
    }
    static ULONG STDMETHODCALLTYPE Ref(FakeQueue *) {
        return 2;
    }
    __declspec(noinline) static void STDMETHODCALLTYPE Execute(FakeQueue *, UINT,
                                                               ID3D12CommandList *const *) {
        SetLastError(123);
    }
    __declspec(noinline) static HRESULT STDMETHODCALLTYPE Signal(FakeQueue *, ID3D12Fence *,
                                                                 UINT64 value) {
        return HRESULT(value + 1);
    }
    __declspec(noinline) static HRESULT STDMETHODCALLTYPE Wait(FakeQueue *, ID3D12Fence *,
                                                               UINT64 value) {
        return HRESULT(value + 2);
    }
    FakeQueue() : v(table.data()) {
        table[0] = reinterpret_cast<void *>(&QI);
        table[1] = table[2] = reinterpret_cast<void *>(&Ref);
        table[10] = reinterpret_cast<void *>(&Execute);
        table[14] = reinterpret_cast<void *>(&Signal);
        table[15] = reinterpret_cast<void *>(&Wait);
    }
    ID3D12CommandQueue *Get() {
        return reinterpret_cast<ID3D12CommandQueue *>(this);
    }
};
static void Exercise(FakeList &list, bool internal) {
    forwarded.clear();
    callback_count = 0;
    D3D12_RESOURCE_BARRIER batch[3]{};
    batch[0] = rrsparsecopy::Transition(nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        D3D12_RESOURCE_STATE_COPY_SOURCE);
    batch[1] = rrsparsecopy::Transition(nullptr, D3D12_RESOURCE_STATE_COMMON,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
    batch[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr),
           done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Need(ready && done, "CPU synchronization events");
    // Exactly the production submission lock, retained until After. The worker
    // takes the real pool mutex just as Snapshot/Apply do while recording copies.
    LiveRR().Before(nullptr, 0, nullptr);
    std::thread recording([&] {
        auto record = [&] {
            rrsparsecopy::RetirementTest::WithLock(SparsePool(), [&] {
                SetEvent(ready);
                list.Get()->ResourceBarrier(3, batch);
            });
        };
        if (internal) {
            rrinternal::Scope outer;
            {
                rrinternal::Scope inner;
                rrcompute::Tracker::Internal state;
                record();
            }
            Need(rrinternal::active != 0, "nested guard restores outer scope");
        } else
            record();
        SetEvent(done);
    });
    const bool started = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0;
    const bool finished = WaitForSingleObject(done, 750) == WAIT_OBJECT_0;
    // Release even on the old broken code so the failing test never hangs.
    LiveRR().After(nullptr);
    recording.join();
    CloseHandle(ready);
    CloseHandle(done);
    Need(started, "recording thread acquired real pool mutex");
    Need(internal ? finished : !finished,
         internal ? "private copy observer blocks on RR controller: reproduces reverse lock order"
                  : "external transition must still reach RR controller");
    Need(forwarded.size() == 3 && callback_count == 2,
         "three original barriers forwarded once; two transition callbacks");
    for (unsigned i = 0; i < 3; ++i)
        Need(memcmp(&forwarded[i], &batch[i], sizeof(batch[i])) == 0,
             "native barrier contents/order unchanged");
    Need(!rrinternal::active, "internal guard remains thread-local");
    SparsePool().Poll(); // Pool mutex released after recording; all slots empty.
}
int main() {
    try {
        FakeList list;
        FakeQueue queue;
        rrbuffer::Inspector inspector;
        inspector.after_transition = Post;
        inspector.observe_transitions = true;
        Need(inspector.AttachQueue(queue.Get()) && inspector.AttachList(list.Get()),
             "actual Inspector hooks attach to CPU-only COM surfaces");
        Exercise(list, true);
        Exercise(list, false);
        Exercise(list, true);
        inspector.RemoveForTest();
        puts("PASS: private copy transitions complete while RR lock is held; external transitions "
             "still observe RR; real Inspector forwards all barriers exactly once in order; "
             "nested/thread-local guard intact; no GPU work");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

#include "../src/rr_buffer_inspector.h"
#include "../src/rr_compute_state.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static void Check(bool value, const char *label) {
    if (!value) {
        printf("FAIL: %s\n", label);
        exit(1);
    }
}
static std::vector<unsigned> events;
static ID3D12GraphicsCommandList *list = reinterpret_cast<ID3D12GraphicsCommandList *>(0x1234);
static ID3D12CommandQueue *queue = reinterpret_cast<ID3D12CommandQueue *>(0x4321);
static bool throw_observer{};
static void STDMETHODCALLTYPE Barrier(ID3D12GraphicsCommandList *cl, UINT n,
                                      const D3D12_RESOURCE_BARRIER *b) {
    Check(cl == list, "barrier identity");
    for (UINT i = 0; i < n; ++i)
        events.push_back(10 + b[i].Transition.Subresource);
    SetLastError(73);
}
static void STDMETHODCALLTYPE Execute(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *l) {
    Check(q == queue && n == 1 && l[0] == list, "submission identity");
    events.push_back(2);
    SetLastError(74);
}
namespace rrbuffer {
struct InspectorTestAccess {
    static void Run() {
        Inspector inspector;
        Inspector::active = &inspector;
        inspector.slots[Inspector::Barrier][0].original = reinterpret_cast<void *>(&Barrier);
        inspector.slots[Inspector::Execute][0].original = reinterpret_cast<void *>(&Execute);
        inspector.execution_ready = true;
        inspector.bindings.try_emplace(list);
        inspector.on_barriers = [](ID3D12GraphicsCommandList *, UINT,
                                   const D3D12_RESOURCE_BARRIER *) {
            events.push_back(1);
            SetLastError(99);
            if (throw_observer)
                throw 1;
        };
        inspector.after_transition = [](ID3D12GraphicsCommandList *,
                                        const D3D12_RESOURCE_BARRIER &b, BarrierFn) {
            events.push_back(20 + b.Transition.Subresource);
            SetLastError(98);
            if (throw_observer)
                throw 1;
        };
        D3D12_RESOURCE_BARRIER b[2]{};
        b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.Subresource = 1;
        inspector.observe_transitions = true;
        for (bool fail : {false, true}) {
            throw_observer = fail;
            events.clear();
            Inspector::OnBarrier<0>(list, 2, b);
            Check(events == std::vector<unsigned>{1, 10, 20, 11, 21},
                  "each barrier forwarded once before callback despite exception");
            Check(GetLastError() == 73, "barrier result error preserved");
        }
        for (unsigned context = 0; context < 3; ++context) {
            inspector.bindings[list].in_pass = context == 0;
            inspector.bindings[list].predicated = context == 1;
            if (context == 2)
                inspector.bindings.erase(list);
            events.clear();
            Inspector::OnBarrier<0>(list, 2, b);
            Check(events == std::vector<unsigned>{1, 10, 11},
                  "unsafe or unknown output context skips posttransition copy");
        }
        inspector.on_execute = [](ID3D12CommandQueue *, UINT,
                                  ID3D12CommandList *const *) -> uint64_t {
            events.push_back(1);
            SetLastError(99);
            if (throw_observer)
                throw 1;
            return 17;
        };
        inspector.on_execute_end = [](ID3D12CommandQueue *, uint64_t token) {
            Check(token == (throw_observer ? 0 : 17), "submission token retained");
            events.push_back(3);
            SetLastError(99);
            if (throw_observer)
                throw 1;
        };
        ID3D12CommandList *lists[] = {list};
        for (bool fail : {false, true}) {
            throw_observer = fail;
            events.clear();
            Inspector::OnExecute<0>(queue, 1, lists);
            Check(events == std::vector<unsigned>{1, 2, 3},
                  "submission once between observers, including failures");
            Check(GetLastError() == 74, "submission error preserved");
        }
        Inspector::active = nullptr;
    }
};
} // namespace rrbuffer
int main() {
    rrbuffer::InspectorTestAccess::Run();
    puts("PASS: CPU barrier/submission forwarding, exceptions, output exclusion and errors; "
         "compute header compiled");
}

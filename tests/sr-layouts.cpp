#include "../src/rr_sparse_copy.h"
#include <cassert>
struct Resource {
    void **v;
    void *table[8]{};
    UINT64 identity{};
    static HRESULT STDMETHODCALLTYPE GetPrivate(Resource *r, REFGUID, UINT *n, void *p) {
        if (!r->identity)
            return E_FAIL;
        assert(*n == 8);
        memcpy(p, &r->identity, 8);
        return S_OK;
    }
    static HRESULT STDMETHODCALLTYPE Set(Resource *r, REFGUID, UINT n, const void *p) {
        r->identity = 0;
        if (n == 8)
            memcpy(&r->identity, p, 8);
        return S_OK;
    }
    Resource() : v(table) {
        table[3] = reinterpret_cast<void *>(&GetPrivate);
        table[4] = reinterpret_cast<void *>(&Set);
    }
    ID3D12Resource *Get() {
        return reinterpret_cast<ID3D12Resource *>(this);
    }
};
int main() {
    Resource a, b, untouched;
    rrsparsecopy::Layouts layouts;
    auto cl = reinterpret_cast<ID3D12GraphicsCommandList *>(1);
    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto transition = rrsparsecopy::Transition(a.Get(), D3D12_RESOURCE_STATE_COMMON, read);
    auto second = rrsparsecopy::Transition(b.Get(), D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_STATES state{};
    layouts.Observe(cl, 1, &transition);
    layouts.Observe(cl, 1, &second);
    assert(layouts.Get(cl, a.Get(), state) && state == read);
    assert(layouts.GetUnaliased(cl, a.Get(), state) && state == read);
    using Alias = rrsparsecopy::Layouts::Alias;
    auto kind = Alias::Named;
    assert(layouts.Get(cl, a.Get(), state, kind) && state == read && kind == Alias::None);
    D3D12_RESOURCE_BARRIER alias{};
    alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    alias.Aliasing = {a.Get(), b.Get()};
    layouts.Observe(cl, 1, &alias);
    // Aliasing changes memory usage, not either object's transition state.
    // This tracker does not certify active allocation or valid contents.
    assert(layouts.Get(cl, a.Get(), state) && state == read);
    assert(layouts.Get(cl, b.Get(), state) && state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // An indirect draw may execute zero draws: a retained binding/state cannot
    // authorize copying contents after its allocation was aliased.
    assert(!layouts.GetUnaliased(cl, a.Get(), state));
    assert(!layouts.GetUnaliased(cl, b.Get(), state));
    assert(!layouts.Get(cl, untouched.Get(), state));
    assert(!layouts.GetUnaliased(cl, untouched.Get(), state));
    assert(layouts.Get(cl, a.Get(), state, kind) && kind == Alias::Named);
    assert(!layouts.Get(cl, untouched.Get(), state, kind));
    layouts.Observe(cl, 1, &transition);
    assert(layouts.GetUnaliased(cl, a.Get(), state) && state == read);
    assert(layouts.Get(cl, a.Get(), state, kind) && kind == Alias::None);
    alias.Aliasing = {};
    layouts.Observe(cl, 1, &alias);
    assert(layouts.Get(cl, a.Get(), state) && state == read);
    assert(!layouts.GetUnaliased(cl, a.Get(), state));
    // A barrier with a null side covers every resource without naming one.
    assert(layouts.Get(cl, a.Get(), state, kind) && state == read && kind == Alias::Any);
    assert(layouts.Get(cl, b.Get(), state, kind) && kind == Alias::Named); // still
    layouts.Observe(cl, 1, &transition);
    alias.Aliasing = {nullptr, b.Get()};
    layouts.Observe(cl, 1, &alias);
    assert(layouts.Get(cl, a.Get(), state, kind) && kind == Alias::Any);
    layouts.Observe(cl, 1, &transition);
    alias.Aliasing = {nullptr, a.Get()};
    layouts.Observe(cl, 1, &alias);
    assert(layouts.Get(cl, a.Get(), state) && state == read);
    assert(!layouts.GetUnaliased(cl, a.Get(), state));
    assert(layouts.Get(cl, a.Get(), state, kind) && kind == Alias::Named);
    transition.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    layouts.Observe(cl, 1, &transition);
    assert(!layouts.Get(cl, a.Get(), state));
    assert(!layouts.GetUnaliased(cl, a.Get(), state));
    transition.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    layouts.Observe(cl, 1, &transition);
    transition.Transition.Subresource = 0;
    layouts.Observe(cl, 1, &transition);
    assert(!layouts.Get(cl, a.Get(), state));
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    layouts.Observe(cl, 1, &transition);
    layouts.Reset(cl);
    assert(!layouts.Get(cl, a.Get(), state));
    assert(!layouts.GetUnaliased(cl, a.Get(), state));
    layouts.Observe(cl, 1, &transition);
    a.identity = 0;
    assert(!layouts.Get(cl, a.Get(), state));
    assert(!layouts.GetUnaliased(cl, a.Get(), state));
    auto replacement = rrsparsecopy::Transition(a.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    layouts.Observe(cl, 1, &replacement);
    assert(layouts.Get(cl, a.Get(), state) && state == D3D12_RESOURCE_STATE_COPY_SOURCE);
    assert(layouts.GetUnaliased(cl, a.Get(), state) && state == D3D12_RESOURCE_STATE_COPY_SOURCE);

    // Status, for SR replacement: an input no barrier on the list named keeps the state it
    // entered the list with; any other unknown state stays untracked.
    using Seen = rrsparsecopy::Layouts::Seen;
    Resource c, d;
    rrsparsecopy::Layouts status;
    auto list = reinterpret_cast<ID3D12GraphicsCommandList *>(2);
    assert(status.Status(list, c.Get(), state) == Seen::Untouched); // no barrier yet
    auto pixel = rrsparsecopy::Transition(c.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    status.Observe(list, 1, &pixel);
    assert(status.Status(list, c.Get(), state) == Seen::Known &&
           state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    assert(status.Status(list, d.Get(), state) == Seen::Untouched);
    auto plane = rrsparsecopy::Transition(d.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, read);
    plane.Transition.Subresource = 0;
    status.Observe(list, 1, &plane);
    assert(status.Status(list, d.Get(), state) == Seen::Untracked);
    assert(!status.Get(list, d.Get(), state));
    plane.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    status.Observe(list, 1, &plane);
    assert(status.Status(list, d.Get(), state) == Seen::Known && state == read);
    pixel.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    status.Observe(list, 1, &pixel);
    assert(status.Status(list, c.Get(), state) == Seen::Untracked);
    pixel.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    status.Observe(list, 1, &pixel);
    c.identity = 0; // a new resource at the same address
    assert(status.Status(list, c.Get(), state) == Seen::Untracked);
    // Barriers the tracker could not see (enhanced barriers, an observer failure) leave
    // nothing untouched on that list until it is reset.
    status.Lose(list);
    assert(status.Status(list, d.Get(), state) == Seen::Untracked);
    assert(status.Status(list, untouched.Get(), state) == Seen::Untracked);
    status.Observe(list, 1, &plane);
    assert(status.Get(list, d.Get(), state) && state == read); // Get tracks on as before
    assert(status.Status(list, d.Get(), state) == Seen::Untracked);
    status.Reset(list);
    assert(status.Status(list, d.Get(), state) == Seen::Untouched);
    status.Observe(list, 4097, &plane);
    assert(status.Status(list, d.Get(), state) == Seen::Untracked);
    assert(status.Status(nullptr, d.Get(), state) == Seen::Untracked);
}

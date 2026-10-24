#pragma once
#include "rr_bindings.h"
#include <d3d12.h>
#include <string>
#include <span>

namespace rrbindings {
// Per-object token, rather than its address: destroyed/recreated heaps must not
// inherit old descriptor contents. This private datum does not alter GPU state.
inline uint64_t HeapIdentity(ID3D12DescriptorHeap *heap) {
    static constexpr GUID key = {
        0x37748bce, 0x2ca2, 0x447e, {0x9d, 0x66, 0xa9, 0xcb, 0x23, 0x30, 0xfd, 0xb8}};
    static std::mutex identity_mutex;
    static uint64_t next = 0;
    std::lock_guard lock(identity_mutex);
    uint64_t value = 0;
    UINT size = sizeof(value);
    const HRESULT result = heap->GetPrivateData(key, &size, &value);
    if (SUCCEEDED(result))
        return size == sizeof(value) ? value : 0;
    if (result != DXGI_ERROR_NOT_FOUND)
        return 0;
    value = ++next;
    return SUCCEEDED(heap->SetPrivateData(key, sizeof(value), &value)) ? value : 0;
}
inline Slot Resolve(device *d, descriptor_table table, uint32_t binding, uint32_t array = 0) {
    if (!table.handle || array)
        return {};
    descriptor_heap heap{};
    uint32_t base = 0;
    d->get_descriptor_heap_offset(table, 0, 0, &heap, &base);
    if (!heap.handle)
        return {};
    auto *native = reinterpret_cast<ID3D12DescriptorHeap *>(heap.handle);
    const auto desc = native->GetDesc();
    const uint64_t offset = uint64_t(base) + binding;
    if (offset >= desc.NumDescriptors)
        return {};
    return {HeapIdentity(native), uint32_t(offset)};
}
inline Value Describe(device *d, resource_view view) {
    if (!view.handle)
        return {};
    Value value;
    const auto resource = d->get_resource_from_view(view);
    value.known = true;
    value.resource = resource.handle;
    if (!resource.handle)
        return value;
    const auto rd = d->get_resource_desc(resource);
    const auto vd = d->get_resource_view_desc(view);
    value.dimension = uint32_t(rd.type);
    value.format = rd.type == resource_type::buffer ? 0 : uint32_t(rd.texture.format);
    value.width = rd.type == resource_type::buffer ? rd.buffer.size : rd.texture.width;
    value.height = rd.type == resource_type::buffer ? 0 : rd.texture.height;
    value.view_format = uint32_t(vd.format);
    value.view_type = uint32_t(vd.type);
    if (rd.type == resource_type::buffer && vd.type == resource_view_type::buffer) {
        value.buffer = true;
        // The tested D3D12 ReShade runtime reports raw-buffer FirstElement and
        // NumElements here (verified at nonzero offset against the actual game DLL).
        // Convert its R32_TYPELESS elements to bytes; never treat a structured union
        // or an untested typed buffer as a byte range.
        if (d->get_api() == device_api::d3d12 && vd.format == format::r32_typeless &&
            vd.buffer.offset <= UINT64_MAX / 4 && vd.buffer.size <= UINT64_MAX / 4) {
            value.byte_offset = vd.buffer.offset * 4;
            value.byte_size = vd.buffer.size * 4;
        }
    }
    if (vd.type != resource_view_type::buffer &&
        vd.type != resource_view_type::acceleration_structure) {
        value.first_level = vd.texture.first_level;
        value.levels = vd.texture.levels;
        value.first_layer = vd.texture.first_layer;
        value.layers = vd.texture.layers;
    }
    return value;
}
struct __declspec(uuid("1643a47d-d80a-4767-8d76-3c4b9e2717ec")) DeviceState {
    Tracker tracker;
    Tracker graphics{1048576, shader_stage::pixel};
    std::atomic<bool> reported{};
};
inline DeviceState *State(device *d) {
    return d->get_api() == device_api::d3d12 ? d->get_private_data<DeviceState>() : nullptr;
}
} // namespace rrbindings

// Included inside Controller. deque growth preserves every live Slot address.
private:
// The persistent term of the pool projections: the persistent frame plus every slot's ray budget
// buffers, which are optional (never multiplied into the per-slot size) but allocated, so they stay
// inside budget.limit. False on overflow.
bool PersistentBytes(UINT64 &total) const {
    total = persistent.bytes;
    for (const auto &s : slots) {
        if (s.gpu.ray_bytes > UINT64_MAX - total)
            return false;
        total += s.gpu.ray_bytes;
    }
    return true;
}
bool ReservePool(size_t count, const rreval::Options *pending = nullptr,
                 const D3D12_RESOURCE_DESC *native_depth = nullptr, UINT guide_width = 0,
                 UINT guide_height = 0, UINT64 extra_persistent = 0) {
    UINT64 base{}, depth{}, sss{}, glass{}, output{};
    auto allocation = [&](const D3D12_RESOURCE_DESC &d) {
        return pipelines.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
    };
    for (const auto &s : slots) {
        UINT64 b{}, d{};
        if (!SlotReservationParts(pipelines.device.Get(), s.gpu, budget.limit, b, d) ||
            b < s.gpu.sss.bytes || b - s.gpu.sss.bytes < s.gpu.transparency.bytes ||
            b - s.gpu.sss.bytes - s.gpu.transparency.bytes < s.gpu.ray_bytes)
            return false;
        b -= s.gpu.sss.bytes + s.gpu.transparency.bytes + s.gpu.ray_bytes;
        if (s.gpu.rr_output) {
            const auto n = allocation(s.gpu.rr_output->GetDesc());
            if (!n || n == UINT64_MAX || n > b)
                return false;
            b -= n;
            output = (std::max)(output, n);
        }
        base = (std::max)(base, b);
        depth = (std::max)(depth, d);
        sss = (std::max)(sss, s.gpu.sss.bytes);
        glass = (std::max)(glass, s.gpu.transparency.bytes);
    }
    if (guide_width || guide_height) {
        const auto bytes =
            GpuFrame::RequiredPrepassBytes(pipelines.device.Get(), guide_width, guide_height);
        if (!bytes)
            return false;
        base = (std::max)(base, bytes);
    }
    if (pending) {
        const auto n = allocation(
            TextureDesc(pending->out_width, pending->out_height, DXGI_FORMAT_R16G16B16A16_FLOAT));
        if (!n || n == UINT64_MAX)
            return false;
        output = (std::max)(output, n);
        if (native_depth && native_depth->Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
            UINT64 bytes{};
            pipelines.device->GetCopyableFootprints(native_depth, 0, 1, 0, nullptr, nullptr,
                                                    nullptr, &bytes);
            auto depth_desc =
                TextureDesc(UINT(native_depth->Width), native_depth->Height, DXGI_FORMAT_R32_FLOAT);
            depth_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            const auto texture = allocation(depth_desc);
            const auto buffer = bytes ? allocation(rrprivate::Buffer(bytes)) : 0;
            if (!texture || !buffer || texture == UINT64_MAX || buffer == UINT64_MAX ||
                texture > UINT64_MAX - buffer)
                return false;
            depth = (std::max)(depth, texture + buffer);
        }
    }
    UINT64 required{}, kept{};
    if (!PersistentBytes(kept) || extra_persistent > UINT64_MAX - kept ||
        output > UINT64_MAX - base ||
        !rrtrans::RequiredSlots(count, base + output, depth, sss, glass, 0, kept + extra_persistent,
                                required))
        return false;
    return budget.Reserve(pipelines.device.Get(), required);
}
Slot *GrowPool() {
    if (!initialized || slots.size() >= UINT_MAX || !ReservePool(slots.size() + 1))
        return nullptr;
    slots.emplace_back();
    return &slots.back();
}

public:

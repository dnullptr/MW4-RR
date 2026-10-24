#pragma once
#include <nvsdk_ngx.h>

namespace rrsr {
// Temporarily reset native SR history after one or more skipped evaluations.
// Preserve both the caller's value and the public getter/setter type.
class ResetParameter {
    NVSDK_NGX_Parameter *parameters{};
    int signed_value{};
    unsigned unsigned_value{};
    bool use_unsigned{}, changed{}, valid{};
    bool Readback(int expected) const {
        if (use_unsigned) {
            unsigned value{};
            return NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Reset, &value)) &&
                   value == unsigned(expected);
        }
        int value{};
        return NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Reset, &value)) &&
               value == expected;
    }
    void Write(int value) {
        if (use_unsigned)
            parameters->Set(NVSDK_NGX_Parameter_Reset, unsigned(value));
        else
            parameters->Set(NVSDK_NGX_Parameter_Reset, value);
    }

  public:
    explicit ResetParameter(const NVSDK_NGX_Parameter *p)
        : parameters(const_cast<NVSDK_NGX_Parameter *>(p)) {
        if (!p)
            return;
        if (NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_Reset, &signed_value)))
            valid = signed_value == 0 || signed_value == 1;
        else if (NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_Reset, &unsigned_value))) {
            use_unsigned = true;
            valid = unsigned_value <= 1;
        }
    }
    ResetParameter(const ResetParameter &) = delete;
    ~ResetParameter() {
        if (changed) {
            try {
                Write(use_unsigned ? int(unsigned_value) : signed_value);
            } catch (...) {
            }
        }
    }
    bool Ready() {
        if (!valid)
            return false;
        const int value = use_unsigned ? int(unsigned_value) : signed_value;
        changed = true;
        Write(value == 0 ? 1 : 0);
        const bool writable = Readback(value == 0 ? 1 : 0);
        Write(value);
        const bool restored = Readback(value);
        changed = !restored;
        return writable && restored;
    }
    bool Force() {
        if (!valid)
            return false;
        changed = true;
        Write(1);
        return Readback(1);
    }
};
} // namespace rrsr

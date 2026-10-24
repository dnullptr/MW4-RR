#pragma once
#include <nvsdk_ngx_params.h>
#include <map>
#include <string>
#include <variant>
// Typed public API boundary fixture: mismatched setter/getter types are errors.
struct RecordingParams : NVSDK_NGX_Parameter {
    using Value = std::variant<unsigned long long, float, double, unsigned, int, ID3D11Resource *,
                               ID3D12Resource *, void *>;
    std::map<std::string, Value> values;
#define FIELD(T)                                                                                   \
    void Set(const char *k, T v) override {                                                        \
        values.insert_or_assign(k, Value(v));                                                      \
    }                                                                                              \
    NVSDK_NGX_Result Get(const char *k, T *v) const override {                                     \
        auto it = values.find(k);                                                                  \
        if (it == values.end())                                                                    \
            return NVSDK_NGX_Result_FAIL_InvalidParameter;                                         \
        auto p = std::get_if<T>(&it->second);                                                      \
        if (!p)                                                                                    \
            return NVSDK_NGX_Result_FAIL_InvalidParameter;                                         \
        *v = *p;                                                                                   \
        return NVSDK_NGX_Result_Success;                                                           \
    }
    FIELD(unsigned long long)
    FIELD(float)
    FIELD(double)
    FIELD(unsigned) FIELD(int) FIELD(ID3D11Resource *) FIELD(ID3D12Resource *) FIELD(void *)
#undef FIELD
        void Reset() override {
        values.clear();
    }
};

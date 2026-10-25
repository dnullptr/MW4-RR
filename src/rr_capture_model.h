#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
namespace rrcapture {
// Device-free model of an RR input capture: what is saved, the files, the manifest and the
// sequence that finds 8 consecutive delivered frames. The D3D12 side lives elsewhere.
inline constexpr unsigned Frames = 8, MaxAttempts = 600;
enum Input : unsigned {
    Color,
    Depth,
    Motion,
    NormalRoughness,
    DiffuseAlbedo,
    SpecularAlbedo,
    Status,
    SssGuide,
    ColorBeforeTransparency,
    ResponsivityMask,
    SpecularHitDistance,
    RrOutput,
    InputCount
};
inline constexpr const char *names[InputCount] = {"color",
                                                  "depth",
                                                  "motion",
                                                  "normal_roughness",
                                                  "diffuse_albedo",
                                                  "specular_albedo",
                                                  "status",
                                                  "sss_guide",
                                                  "color_before_transparency",
                                                  "responsivity_mask",
                                                  "specular_hit_distance",
                                                  "rr_output"};

struct FormatInfo {
    unsigned dxgi;
    const char *name;
    unsigned bytes;
    const char *layout;
};
inline constexpr FormatInfo formats[] = {
    {2, "R32G32B32A32_FLOAT", 16, "4 x float32 RGBA"},
    {10, "R16G16B16A16_FLOAT", 8, "4 x float16 RGBA"},
    {16, "R32G32_FLOAT", 8, "2 x float32"},
    {26, "R11G11B10_FLOAT", 4,
     "uint32: R bits 0-10 and G bits 11-21 (6-bit mantissa, 5-bit exponent), B bits 22-31 "
     "(5-bit mantissa, 5-bit exponent); unsigned, exponent bias 15"},
    {34, "R16G16_FLOAT", 4, "2 x float16"},
    {41, "R32_FLOAT", 4, "float32"},
    {42, "R32_UINT", 4, "uint32"},
    {54, "R16_FLOAT", 2, "float16"},
};
inline const FormatInfo *Format(unsigned dxgi) {
    for (const FormatInfo &f : formats)
        if (f.dxgi == dxgi)
            return &f;
    return nullptr;
}
inline std::string FormatName(unsigned dxgi) {
    const FormatInfo *f = Format(dxgi);
    return f ? f->name : "DXGI_" + std::to_string(dxgi);
}

struct Region { // One image inside a readback buffer.
    bool present{};
    unsigned format{}, width{}, height{}, bytes_per_pixel{};
    std::uint64_t offset{}, pitch{}; // Placement inside the buffer.
    unsigned source_x{}, source_y{}, source_width{}, source_height{};
};
struct Camera {
    float position[3]{}, right[3]{}, up[3]{}, forward[3]{}, jitter[2]{}, motion_scale[2]{},
        projection[16]{}, inverse[16]{}, near_plane{}, far_plane{}, fov{}, aspect{};
    unsigned depth_inverted{}, camera_motion{}, motion3d{}, reset{}, orthographic{},
        motion_dilated{}, motion_jittered{};
};
struct Meta {
    unsigned serial{}, sl_frame{}, viewport{}, reset_reasons{};
    bool delivered{}, reset{}, replaced_sr{};
    std::array<unsigned, 4> stats{}; // Bad frame, copied frame, isolated guide px, sanitized SSS.
    float jitter[2]{}, motion_scale[2]{}, pre_exposure{}, exposure_scale{};
    unsigned color_subrect[2]{}, depth_subrect[2]{}, motion_subrect[2]{};
    float world_to_view[16]{}, view_to_clip[16]{};
    Camera camera;
    double cpu_time_ms{};
};
struct Header {
    std::string created;
    unsigned render_width{}, render_height{}, output_width{}, output_height{}, preset{},
        guide_test{}, lighting_input{};
    unsigned ray_selection{}; // rrapply variant of the last VRRT apply dispatch; 0 = native.
    int quality{};
    bool sss_enabled{}, transparency_enabled{}, specular_hit_enabled{}, tolerate_isolated{},
        responsivity_enabled{}, sky_depth_test{};
    float responsivity_strength{};
};
template <class Buffer> struct Frame {
    Buffer buffer{};
    unsigned width{}, height{};
    unsigned session{}; // The Sequence::session this frame was recorded in.
    std::uint64_t bytes{};
    std::array<Region, InputCount> regions{};
    Meta meta;
};

inline std::string FileName(unsigned index, unsigned input) {
    return "frame" + std::to_string(index) + "_" + names[input] + ".bin";
}
// Calls write(row, row_bytes) once per row, top row first, with the buffer's row padding removed.
template <class Write> void PackRows(const unsigned char *base, const Region &r, Write write) {
    const std::uint64_t row_bytes = std::uint64_t(r.width) * r.bytes_per_pixel;
    for (unsigned y = 0; y < r.height; ++y)
        write(base + r.offset + y * r.pitch, row_bytes);
}
// True when every byte PackRows reads from a present, non-empty region lies within bytes.
inline bool Fits(const Region &r, std::uint64_t bytes) {
    const std::uint64_t row = std::uint64_t(r.width) * r.bytes_per_pixel;
    if (!r.present || !row || !r.height || r.pitch < row || r.offset > bytes ||
        row > bytes - r.offset)
        return false;
    return r.height == 1 || r.pitch <= (bytes - r.offset - row) / (r.height - 1);
}

inline void JsonNumber(std::string &out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char text[32]; // Locale independent, and the same text as printf("%.9g").
    const auto r = std::to_chars(text, text + sizeof text, v, std::chars_format::general, 9);
    if (r.ec == std::errc{})
        out.append(text, r.ptr);
    else
        out += "null";
}
inline void JsonFixed(std::string &out, double v) { // Three decimals, locale independent.
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char text[320]; // Room for any finite double.
    const auto r = std::to_chars(text, text + sizeof text, v, std::chars_format::fixed, 3);
    if (r.ec == std::errc{})
        out.append(text, r.ptr);
    else
        out += "null";
}
inline void JsonString(std::string &out, const char *s) {
    out += '"';
    for (; *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += *s;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char text[8];
            std::snprintf(text, sizeof text, "\\u%04x", c);
            out += text;
        } else {
            out += *s;
        }
    }
    out += '"';
}
inline void JsonUint(std::string &out, std::uint64_t v) {
    out += std::to_string(v);
}
inline void JsonBool(std::string &out, bool v) {
    out += v ? "true" : "false";
}
inline void JsonFloats(std::string &out, const float *v, std::size_t count) {
    out += '[';
    for (std::size_t i = 0; i < count; ++i) {
        if (i)
            out += ", ";
        JsonNumber(out, v[i]);
    }
    out += ']';
}
inline void JsonUints(std::string &out, const unsigned *v, std::size_t count) {
    out += '[';
    for (std::size_t i = 0; i < count; ++i) {
        if (i)
            out += ", ";
        JsonUint(out, v[i]);
    }
    out += ']';
}
// Writes the "key": prefix of each member of an object, with sep between members.
class JsonFields {
    std::string &out;
    const std::string sep;
    bool first = true;

  public:
    JsonFields(std::string &o, std::string separator) : out(o), sep(std::move(separator)) {}
    std::string &Key(const char *key) {
        if (!first)
            out += sep;
        first = false;
        JsonString(out, key);
        out += ": ";
        return out;
    }
};
inline void JsonRegion(std::string &o, unsigned index, unsigned input, const Region &r) {
    if (!r.present) {
        o += "null";
        return;
    }
    const unsigned offset[2] = {r.source_x, r.source_y},
                   size[2] = {r.source_width, r.source_height};
    o += '{';
    JsonFields f(o, ", ");
    JsonString(f.Key("file"), FileName(index, input).c_str());
    JsonString(f.Key("format"), FormatName(r.format).c_str());
    JsonUint(f.Key("dxgi_format"), r.format);
    JsonUint(f.Key("width"), r.width);
    JsonUint(f.Key("height"), r.height);
    JsonUint(f.Key("bytes_per_pixel"), r.bytes_per_pixel);
    JsonUints(f.Key("source_offset"), offset, 2);
    JsonUints(f.Key("source_size"), size, 2);
    o += '}';
}
inline void JsonFrame(std::string &o, unsigned index, const Meta &m,
                      const std::array<Region, InputCount> &regions) {
    o += "{\n      ";
    JsonFields f(o, ",\n      ");
    JsonUint(f.Key("index"), index);
    JsonUint(f.Key("serial"), m.serial);
    JsonUint(f.Key("sl_frame"), m.sl_frame);
    JsonUint(f.Key("viewport"), m.viewport);
    JsonBool(f.Key("delivered"), m.delivered);
    JsonBool(f.Key("reset"), m.reset);
    JsonUint(f.Key("reset_reasons"), m.reset_reasons);
    JsonBool(f.Key("replaced_sr"), m.replaced_sr);
    f.Key("stats");
    o += '{';
    JsonFields st(o, ", ");
    JsonUint(st.Key("bad_frame"), m.stats[0]);
    JsonUint(st.Key("copied_frame"), m.stats[1]);
    JsonUint(st.Key("isolated_guide_pixels"), m.stats[2]);
    JsonUint(st.Key("sanitized_sss_samples"), m.stats[3]);
    o += '}';
    JsonFloats(f.Key("jitter"), m.jitter, 2);
    JsonFloats(f.Key("motion_vector_scale"), m.motion_scale, 2);
    JsonNumber(f.Key("pre_exposure"), m.pre_exposure);
    JsonNumber(f.Key("exposure_scale"), m.exposure_scale);
    JsonUints(f.Key("color_subrect"), m.color_subrect, 2);
    JsonUints(f.Key("depth_subrect"), m.depth_subrect, 2);
    JsonUints(f.Key("motion_subrect"), m.motion_subrect, 2);
    JsonFloats(f.Key("world_to_view"), m.world_to_view, 16);
    JsonFloats(f.Key("view_to_clip"), m.view_to_clip, 16);
    const Camera &c = m.camera;
    f.Key("camera");
    o += "{\n        ";
    JsonFields cf(o, ",\n        ");
    JsonFloats(cf.Key("position"), c.position, 3);
    JsonFloats(cf.Key("right"), c.right, 3);
    JsonFloats(cf.Key("up"), c.up, 3);
    JsonFloats(cf.Key("forward"), c.forward, 3);
    JsonNumber(cf.Key("near"), c.near_plane);
    JsonNumber(cf.Key("far"), c.far_plane);
    JsonNumber(cf.Key("fov"), c.fov);
    JsonNumber(cf.Key("aspect"), c.aspect);
    JsonFloats(cf.Key("jitter"), c.jitter, 2);
    JsonFloats(cf.Key("motion_vector_scale"), c.motion_scale, 2);
    JsonFloats(cf.Key("view_to_clip"), c.projection, 16);
    JsonFloats(cf.Key("clip_to_view"), c.inverse, 16);
    JsonUint(cf.Key("depth_inverted"), c.depth_inverted);
    JsonUint(cf.Key("camera_motion_included"), c.camera_motion);
    JsonUint(cf.Key("motion_vectors_3d"), c.motion3d);
    JsonUint(cf.Key("reset"), c.reset);
    JsonUint(cf.Key("orthographic"), c.orthographic);
    JsonUint(cf.Key("motion_vectors_dilated"), c.motion_dilated);
    JsonUint(cf.Key("motion_vectors_jittered"), c.motion_jittered);
    o += "\n      }";
    JsonFixed(f.Key("cpu_time_ms"), m.cpu_time_ms);
    f.Key("inputs");
    o += "{\n        ";
    JsonFields in(o, ",\n        ");
    for (unsigned i = 0; i < InputCount; ++i)
        JsonRegion(in.Key(names[i]), index, i, regions[i]);
    o += "\n      }\n    }";
}
template <class Buffer>
std::string Manifest(const Header &h, const std::vector<Frame<Buffer>> &frames) {
    std::string o = "{\n  ";
    JsonFields top(o, ",\n  ");
    top.Key("format") += "\"mw4-rr-capture\"";
    top.Key("version") += '1';
    JsonString(top.Key("created"), h.created.c_str());
    JsonUint(top.Key("frame_count"), frames.size());
    JsonUint(top.Key("render_width"), h.render_width);
    JsonUint(top.Key("render_height"), h.render_height);
    JsonUint(top.Key("output_width"), h.output_width);
    JsonUint(top.Key("output_height"), h.output_height);
    top.Key("quality") += std::to_string(h.quality);
    JsonUint(top.Key("preset"), h.preset);
    top.Key("settings");
    o += "{\n    ";
    JsonFields s(o, ",\n    ");
    JsonBool(s.Key("sss_enabled"), h.sss_enabled);
    JsonBool(s.Key("transparency_enabled"), h.transparency_enabled);
    JsonBool(s.Key("specular_hit_enabled"), h.specular_hit_enabled);
    JsonBool(s.Key("tolerate_isolated"), h.tolerate_isolated);
    JsonUint(s.Key("lighting_input"), h.lighting_input);
    JsonBool(s.Key("responsivity_enabled"), h.responsivity_enabled);
    JsonNumber(s.Key("responsivity_strength"), h.responsivity_strength);
    JsonUint(s.Key("guide_test"), h.guide_test);
    JsonBool(s.Key("sky_depth_test"), h.sky_depth_test);
    JsonUint(s.Key("ray_selection"), h.ray_selection);
    o += "\n  }";
    top.Key("status_values") +=
        R"({"0": "padding", "1": "ordinary", "2": "sky", "3": "special", "4": "invalid"})";
    top.Key("reset_reason_bits") +=
        R"({"1": "rejected", "2": "cut", "4": "generation", "8": "gap"})";
    top.Key("formats");
    o += "{\n    ";
    JsonFields fm(o, ",\n    ");
    for (const FormatInfo &f : formats) {
        fm.Key(f.name);
        o += '{';
        JsonFields e(o, ", ");
        JsonUint(e.Key("dxgi_format"), f.dxgi);
        JsonUint(e.Key("bytes_per_pixel"), f.bytes);
        JsonString(e.Key("layout"), f.layout);
        o += '}';
    }
    o += "\n  }";
    top.Key("frames") += '[';
    for (std::size_t i = 0; i < frames.size(); ++i) {
        o += i ? ",\n    " : "\n    ";
        JsonFrame(o, unsigned(i), frames[i].meta, frames[i].regions);
    }
    o += frames.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

enum class State { Idle, Armed, Collecting, Writing, Saved, Failed };
// Finds 8 consecutive delivered frames. F supplies buffer, width, height, session and meta; a
// buffer is empty when !buffer. Single threaded: callers serialize access.
// Each Request starts a new session. Callers stamp frame.session = session when they record a
// frame, so a copy still in flight from an earlier session is dropped when it completes.
template <class F> class Sequence {
  public:
    using Buffer = decltype(F::buffer);
    State state = State::Idle;
    unsigned attempts{}, session{};
    std::string message;
    std::vector<Buffer> pool;
    std::vector<F> run, completed;

    bool Request() {
        if (state != State::Idle && state != State::Saved && state != State::Failed)
            return false;
        attempts = 0;
        message.clear();
        pool.clear();
        run.clear();
        completed.clear();
        ++session;
        state = State::Armed;
        return true;
    }
    bool Active() const {
        return state == State::Armed || state == State::Collecting;
    }
    bool Wants(std::size_t in_flight) const {
        return state == State::Collecting && run.size() + in_flight < Frames;
    }
    void Begin(std::vector<Buffer> buffers) {
        if (state != State::Armed)
            return;
        pool = std::move(buffers);
        state = State::Collecting;
    }
    Buffer Take() {
        if (pool.empty())
            return Buffer{};
        Buffer b = std::move(pool.back());
        pool.pop_back();
        return b;
    }
    void Recycle(Buffer b) {
        if (state == State::Collecting && b)
            pool.push_back(std::move(b));
    }
    bool Count() {
        if (++attempts <= MaxAttempts)
            return true;
        Fail("RR did not deliver 8 consecutive frames");
        return false;
    }
    void Complete(F f) {
        completed.push_back(std::move(f));
    }
    // Takes the finished frames in serial order. Returns the 8 frames of a complete run (and
    // moves to Writing), or nothing.
    std::vector<F> Settle() {
        std::vector<F> queue = std::move(completed), done;
        completed.clear();
        std::stable_sort(queue.begin(), queue.end(),
                         [](const F &a, const F &b) { return a.meta.serial < b.meta.serial; });
        for (F &f : queue) {
            if (f.session != session || state != State::Collecting)
                continue; // From an earlier session, or nothing is collecting: drop it.
            if (!f.meta.delivered) {
                Restart();
                Recycle(std::move(f.buffer));
                continue;
            }
            // A reset frame begins a new RR history, so it can only start a run.
            if (!run.empty() && (f.meta.reset || f.meta.serial != run.back().meta.serial + 1 ||
                                 f.width != run.back().width || f.height != run.back().height))
                Restart();
            run.push_back(std::move(f));
            if (run.size() == Frames) {
                state = State::Writing;
                done = std::move(run);
                run.clear();
            }
        }
        return done;
    }
    void Fail(std::string why) {
        if (state == State::Writing)
            return; // A writer owns this session's outcome; only Finished ends it.
        state = State::Failed;
        message = std::move(why);
        run.clear();
        pool.clear();
        completed.clear();
    }
    void Finished(bool ok, std::string text) {
        if (state != State::Writing)
            return;
        state = ok ? State::Saved : State::Failed;
        message = std::move(text);
    }

  private:
    void Restart() { // The run is broken: its buffers go back to the pool.
        for (F &f : run)
            Recycle(std::move(f.buffer));
        run.clear();
    }
};
} // namespace rrcapture

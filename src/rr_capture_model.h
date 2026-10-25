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
#include "rr_weapon_depth.h"
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
    DisocclusionMask,    // Water history discard test (rr_water.h).
    AnimatedTextureMask, // Water animated texture mask test (rr_water.h).
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
                                                  "disocclusion_mask",
                                                  "animated_texture_mask",
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
    {61, "R8_UNORM", 1, "uint8, 0-255 for 0-1"},
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

// Where the state an input's copy moved it from (and back to) came from: the addon's own resource,
// a state proven for that frame, the state tracker's, or the DLSS input contract's (NPSR).
enum StateSource : unsigned { Addon, Proven, Tracked, Contract, StateSourceCount };
inline constexpr const char *state_sources[StateSourceCount] = {"addon", "proven", "layouts",
                                                                "contract"};
inline const char *StateSourceName(unsigned source) {
    return source < StateSourceCount ? state_sources[source] : "unknown";
}
struct Region { // One image inside a readback buffer.
    bool present{};
    unsigned format{}, width{}, height{}, bytes_per_pixel{};
    std::uint64_t offset{}, pitch{}; // Placement inside the buffer.
    unsigned source_x{}, source_y{}, source_width{}, source_height{};
    // The StateSource of the state the copy moved it from, and that D3D12_RESOURCE_STATES.
    unsigned state_source{}, state_before{};
};
struct Camera {
    float position[3]{}, right[3]{}, up[3]{}, forward[3]{}, jitter[2]{}, motion_scale[2]{},
        projection[16]{}, inverse[16]{}, near_plane{}, far_plane{}, fov{}, aspect{};
    unsigned depth_inverted{}, camera_motion{}, motion3d{}, reset{}, orthographic{},
        motion_dilated{}, motion_jittered{};
};
// Which responsivity mask RR took (Meta::responsivity_source). The water mask also carries the
// SSS values when both are on; the sky edge test replaces both.
enum ResponsivitySource : unsigned { NoMask, SssMask, SkyEdgeMask, WaterMask, SourceCount };
inline constexpr const char *responsivity_sources[SourceCount] = {"none", "sss", "sky_edge",
                                                                  "water"};
inline const char *ResponsivitySourceName(unsigned source) {
    return source < SourceCount ? responsivity_sources[source] : "unknown";
}
// A reset_reasons bit beyond HistoryPolicy's: the color brightness test changed its scale,
// or the contrast test its knee (both transform the color copy RR's history was built on).
inline constexpr unsigned ColorScaleReset = 16;
struct Meta {
    unsigned serial{}, sl_frame{}, viewport{}, reset_reasons{};
    // Specular hit distance tests applied to the sent distance (rrhitcond mode bits:
    // 1 sky and misses far, 2 fill); 0 when it is the game's raw value.
    unsigned hit_distance_test{};
    bool delivered{}, reset{}, replaced_sr{};
    // Water tests (rr_water.h): whether this frame computed the water masks (the settings say
    // which ones RR took), and the water guide mode its guides were converted with.
    bool water_masks{};
    unsigned water_guide{};
    // Depth is linear view depth (weapon depth, rr_weapon_depth.h), not hardware depth, with
    // this mapping: world depth = scale / (hardware depth + offset), sky = sky.
    bool linear_depth{};
    std::array<float, 3> linear_map{}; // scale, offset, sky
    // Tree-noise tests: the color brightness test's factor on RR's color (RR's output is in the
    // same scaled domain; 1 = off), the contrast test's knee (rr_contrast_test.h: the scaled
    // color, and RR's output, are compressed by it; 0 = off), the guide test mode this frame's
    // guides were converted with, the depth sky was lifted to (0 = not lifted; with
    // linear_depth, the hardware depth its sky stands for, which no raw texel equals), and the
    // responsivity mask RR took.
    float color_scale = 1.0f, contrast_knee{}, depth_floor{};
    unsigned guide_test{}, responsivity_source{};
    std::array<unsigned, 4> stats{}; // Bad frame, copied frame, isolated guide px, sanitized SSS.
    float jitter[2]{}, motion_scale[2]{}, pre_exposure{}, exposure_scale{};
    unsigned color_subrect[2]{}, depth_subrect[2]{}, motion_subrect[2]{};
    float world_to_view[16]{}, view_to_clip[16]{};
    Camera camera;
    double cpu_time_ms{};
    // The Lighting input (rr_lighting_source.h) this frame's lighting was prepared with, or
    // MixedLighting: the requested input; a variant that failed to create prepares raw lighting
    // under the same label. lighting_masked: 1 an SSS-masked input had its SSS guide, 0 it fell
    // back to raw lighting, -1 neither (another input, not known, or mixed).
    unsigned lighting_input{};
    int lighting_masked = -1;
};
inline constexpr unsigned MixedLighting = 0xFFFFFFFFu; // The lighting changed within the frame.
// What a frame's first spatial passes noted they prepared, until the frame is captured.
struct Lighting {
    bool noted{}, mixed{};
    unsigned input{};
    int masked = -1;
    void Note(unsigned lobe, int guide) {
        if (!noted) {
            noted = true;
            input = lobe;
            masked = guide;
        } else if (lobe != input || guide != masked) {
            mixed = true;
        }
    }
    // setting: the Lighting input the settings apply, for a frame that noted nothing.
    void Stamp(Meta &m, unsigned setting) const {
        m.lighting_input = mixed ? MixedLighting : noted ? input : setting;
        m.lighting_masked = noted && !mixed ? masked : -1;
    }
};
struct Header {
    std::string created;
    // create_flags: the flags RR was created with (rreval::CreateFlags drops the game's
    // AutoExposure, and DepthInverted with weapon depth's linear depth).
    unsigned render_width{}, render_height{}, output_width{}, output_height{}, preset{},
        guide_test{}, lighting_input{}, create_flags{};
    unsigned ray_selection{}; // rrapply variant of the last VRRT apply dispatch; 0 = native.
    int quality{};
    bool sss_enabled{}, transparency_enabled{}, specular_hit_enabled{}, tolerate_isolated{},
        responsivity_enabled{}, sky_depth_test{}, weapon_depth{};
    float responsivity_strength{};
    // Running water tests (rr_water.h); water_guide is an rrwater::Guide.
    bool water_responsivity{}, water_discard{}, water_animated{};
    float water_strength{}, water_discard_strength{};
    unsigned water_guide{};
    // Sky edge responsivity mask test (rr_sky_edge.h).
    bool sky_edge_enabled{};
    float sky_edge_strength{};
    unsigned sky_edge_radius{};
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
    JsonString(f.Key("state"), StateSourceName(r.state_source));
    JsonUint(f.Key("state_before"), r.state_before);
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
    JsonUint(f.Key("hit_distance_test"), m.hit_distance_test);
    JsonBool(f.Key("water_masks"), m.water_masks);
    JsonUint(f.Key("water_guide"), m.water_guide);
    JsonNumber(f.Key("color_scale"), m.color_scale);
    JsonNumber(f.Key("contrast_knee"), m.contrast_knee);
    JsonUint(f.Key("guide_test"), m.guide_test);
    JsonNumber(f.Key("depth_floor"), m.depth_floor);
    // Weapon depth's mapping (rr_weapon_depth.h; tools/analyze_sky_edges.py undo_tests inverts
    // it), or null for hardware depth.
    f.Key("linear_depth");
    if (m.linear_depth) {
        o += '{';
        JsonFields ld(o, ", ");
        JsonNumber(ld.Key("scale"), m.linear_map[0]);
        JsonNumber(ld.Key("offset"), m.linear_map[1]);
        JsonNumber(ld.Key("sky"), m.linear_map[2]);
        JsonNumber(ld.Key("slice"), rrweapondepth::Slice);
        JsonNumber(ld.Key("knee"), rrweapondepth::Knee);
        o += '}';
    } else
        o += "null";
    JsonString(f.Key("responsivity_source"), ResponsivitySourceName(m.responsivity_source));
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
    JsonUint(f.Key("lighting_input"), m.lighting_input);
    if (m.lighting_masked < 0)
        f.Key("lighting_masked") += "null";
    else
        JsonBool(f.Key("lighting_masked"), m.lighting_masked != 0);
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
    JsonUint(top.Key("create_flags"), h.create_flags);
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
    JsonBool(s.Key("water_responsivity"), h.water_responsivity);
    JsonNumber(s.Key("water_strength"), h.water_strength);
    JsonBool(s.Key("water_discard"), h.water_discard);
    JsonNumber(s.Key("water_discard_strength"), h.water_discard_strength);
    JsonBool(s.Key("water_animated"), h.water_animated);
    JsonUint(s.Key("water_guide"), h.water_guide);
    JsonBool(s.Key("weapon_depth"), h.weapon_depth);
    JsonBool(s.Key("sky_edge_enabled"), h.sky_edge_enabled);
    JsonNumber(s.Key("sky_edge_strength"), h.sky_edge_strength);
    JsonUint(s.Key("sky_edge_radius"), h.sky_edge_radius);
    JsonUint(s.Key("ray_selection"), h.ray_selection);
    o += "\n  }";
    top.Key("status_values") +=
        R"({"0": "padding", "1": "ordinary", "2": "sky", "3": "special", "4": "invalid"})";
    top.Key("reset_reason_bits") +=
        R"({"1": "rejected", "2": "cut", "4": "generation", "8": "gap", "16": "color_scale"})";
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
    // Counts one attempt; the one past the cap fails the capture with why, if given.
    bool Count(const char *why = nullptr) {
        if (++attempts <= MaxAttempts)
            return true;
        Fail(why ? why : "RR did not deliver 8 consecutive frames");
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
            // A reset frame begins a new RR history, so it can only start a run. A run also keeps
            // one lighting input, with or without its SSS guide; a mixed frame joins no run.
            if (!run.empty() && (f.meta.reset || f.meta.serial != run.back().meta.serial + 1 ||
                                 f.width != run.back().width || f.height != run.back().height ||
                                 f.meta.lighting_input == MixedLighting ||
                                 f.meta.lighting_input != run.back().meta.lighting_input ||
                                 f.meta.lighting_masked != run.back().meta.lighting_masked))
                Restart();
            run.push_back(std::move(f));
            if (run.size() == Frames) {
                state = State::Writing;
                done = std::move(run);
                run.clear();
                pool.clear(); // Only the run's buffers stay alive.
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

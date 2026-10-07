export module RendererVK:UboBlock;

import Core;
import Core.glm;
import Settings.Tweaks;

import :Layout;

// THE FRAME UBO: one std140 block, laid out by registration - no C++ struct mirrors it. EVERY value is ONE line, its
// GLSL name, a lambda and its SOURCES:
//
//     ubo.add("fog_density", f.density);                                          // a tweak as it is
//     ubo.add("fog_albedo", [&] { return f.albedo * f.albedoIntensity; }, f.albedo, f.albedoIntensity);
//     ubo.add("sunColor", [this] { return m_skyParams.sunColor * m_skyParams.sunIntensity; }, UboLive);
//
// Sources that are all tweak variables (by reference: their addresses, resolved by TweakRegistry::sourceState) make the
// value LOCKABLE: a GLSL const while every one of them is locked (Renderer::applyUboLocks). UboLive marks a value that
// also reads something that is no tweak (the camera, the clock, a readback): never baked. Moving a value between the two
// is editing its sources. UboPresent is a live value only present() knows (evaluatePresent; its entries are contiguous
// and upload alone). addArray takes a lambda per element index.
//
// The GLSL type comes from the lambda's return type (bool / int -> float, the way the shaders compare them). The block
// packs the entries with std140 rules in registration order; an array's element must be a 16-byte multiple (vec4, uvec4,
// mat4). evaluate() runs every lambda once per frame build, in registration order; buildUboDeclaration
// (UboDeclaration.cpp) makes the GLSL from the same entries: a flat block of u_<name>, each member at its offset.
//
// A lambda runs every frame, long after the registration returns: it captures `[&]` over local REFERENCES to
// long-lived objects only (a reference captured by reference names the object itself) or `this` - never a local value.

namespace UboBlockDetail
{
    using RendererVKLayout::EUboType;

    // A member's std140 layout: its GLSL type, size and base alignment.
    template<typename T> struct Layout;
    template<> struct Layout<float>      { static constexpr EUboType type = EUboType::Float; static constexpr uint32 size = 4,  align = 4; };
    template<> struct Layout<uint32>     { static constexpr EUboType type = EUboType::Uint;  static constexpr uint32 size = 4,  align = 4; };
    template<> struct Layout<glm::vec2>  { static constexpr EUboType type = EUboType::Vec2;  static constexpr uint32 size = 8,  align = 8; };
    template<> struct Layout<glm::vec3>  { static constexpr EUboType type = EUboType::Vec3;  static constexpr uint32 size = 12, align = 16; };
    template<> struct Layout<glm::vec4>  { static constexpr EUboType type = EUboType::Vec4;  static constexpr uint32 size = 16, align = 16; };
    template<> struct Layout<glm::uvec4> { static constexpr EUboType type = EUboType::Uvec4; static constexpr uint32 size = 16, align = 16; };
    template<> struct Layout<glm::mat4>  { static constexpr EUboType type = EUboType::Mat4;  static constexpr uint32 size = 64, align = 16; };

    // The stored type: bool and int travel as floats (the shaders compare them as floats).
    template<typename T> struct Stored { using type = T; };
    template<> struct Stored<bool> { using type = float; };
    template<> struct Stored<int>  { using type = float; };

    struct Eval
    {
        virtual ~Eval() = default;
        virtual void write(uint8* out) const = 0;
    };
    template<typename F>
    struct EvalFn final : Eval
    {
        using T = typename Stored<std::remove_cvref_t<std::invoke_result_t<F&>>>::type;
        explicit EvalFn(F f) : fn(oc::move(f)) {}
        void write(uint8* out) const override
        {
            const T value = static_cast<T>(fn());
            std::memcpy(out, &value, sizeof(T));
        }
        F fn;
    };
    // An array: fn(i) per element, the elements at their std140 stride (their size: a 16-byte multiple).
    template<typename F>
    struct EvalArrayFn final : Eval
    {
        using T = typename Stored<std::remove_cvref_t<std::invoke_result_t<F&, uint32>>>::type;
        EvalArrayFn(F f, uint32 n) : fn(oc::move(f)), count(n) {}
        void write(uint8* out) const override
        {
            for (uint32 i = 0; i < count; ++i)
            {
                const T value = static_cast<T>(fn(i));
                std::memcpy(out + i * sizeof(T), &value, sizeof(T));
            }
        }
        F fn;
        uint32 count;
    };
}

// The source tags: a value that reads something that is no tweak (never baked), and one only present() knows.
export struct UboLiveTag {};
export inline constexpr UboLiveTag UboLive{};
export struct UboPresentTag {};
export inline constexpr UboPresentTag UboPresent{};

export class UboBlock
{
public:
    struct Entry
    {
        oc::string name;                       // the GLSL member is u_<name>
        RendererVKLayout::EUboType type;
        uint32 offset;                         // in the block
        uint32 size;                           // one element's
        uint32 count = 0;                      // an array's length; 0 = not an array
        oc::vector<TweakRegistry::Source> sources;
        oc::unique_ptr<UboBlockDetail::Eval> eval;
        bool live = false;                     // UboLive / UboPresent: never baked
        bool present = false;                  // UboPresent: evaluatePresent only
    };

    // A tweak variable as it is: also its own source.
    template<typename T> requires (!std::is_invocable_v<const T&>)
    void add(const char* name, const T& variable)
    {
        const T* p = &variable;
        add(name, [p] { return *p; }, variable);
    }

    // A computed value and its sources: every tweak variable it reads, or UboLive / UboPresent.
    template<typename F, typename... S> requires std::is_invocable_v<F&>
    void add(const char* name, F fn, const S&... sources)
    {
        using T = typename UboBlockDetail::Stored<std::remove_cvref_t<std::invoke_result_t<F&>>>::type;
        Entry& e = push(name, UboBlockDetail::Layout<T>::type, UboBlockDetail::Layout<T>::size, UboBlockDetail::Layout<T>::align, 0);
        e.eval = oc::make_unique<UboBlockDetail::EvalFn<F>>(oc::move(fn));
        setSources(e, sources...);
    }

    // fn(i) per element (16-byte elements), the sources as add's.
    template<typename F, typename... S> requires std::is_invocable_v<F&, uint32>
    void addArray(const char* name, uint32 count, F fn, const S&... sources)
    {
        using T = typename UboBlockDetail::Stored<std::remove_cvref_t<std::invoke_result_t<F&, uint32>>>::type;
        static_assert(UboBlockDetail::Layout<T>::size % 16 == 0, "std140 array elements: 16-byte multiples only");
        Entry& e = push(name, UboBlockDetail::Layout<T>::type, UboBlockDetail::Layout<T>::size, 16, count);
        e.eval = oc::make_unique<UboBlockDetail::EvalArrayFn<F>>(oc::move(fn), count);
        setSources(e, sources...);
    }

    // Every value but the present ones into the block; the live ones only when asked (the init bake skips them: what
    // they read may not exist yet).
    void evaluate(bool includeLive = true)
    {
        for (const Entry& e : m_entries)
            if (!e.present && (includeLive || !e.live))
                e.eval->write(m_bytes.data() + e.offset);
    }
    // The UboPresent values (present(): known after the begin-frame build), bytes [presentBegin, + presentSize).
    void evaluatePresent()
    {
        for (const Entry& e : m_entries)
            if (e.present)
                e.eval->write(m_bytes.data() + e.offset);
    }
    uint32 presentBegin() const { return m_presentBegin; }
    uint32 presentSize() const { return m_presentEnd - m_presentBegin; }

    const oc::vector<Entry>& entries() const { return m_entries; }
    const uint8* data() const { return m_bytes.data(); }
    uint32 size() const { return (uint32)m_bytes.size(); }

private:
    Entry& push(const char* name, RendererVKLayout::EUboType type, uint32 size, uint32 align, uint32 count)
    {
        for (const Entry& e : m_entries)
            assert(e.name != name && "UboBlock: a GLSL name registered twice");
        Entry& e = m_entries.emplace_back();
        e.name = name;
        e.type = type;
        e.offset = (m_end + align - 1u) & ~(align - 1u);
        e.size = size;
        e.count = count;
        m_end = e.offset + size * oc::max(count, 1u);
        // std140: the block's size rounds to 16; the member after an array or a vec4 starts aligned anyway.
        m_bytes.resize((m_end + 15u) & ~15u, 0);
        return e;
    }

    template<typename... S>
    void setSources(Entry& e, const S&... sources)
    {
        constexpr bool present = (std::is_same_v<S, UboPresentTag> || ...);
        constexpr bool live = present || (std::is_same_v<S, UboLiveTag> || ...);
        e.live = live;
        e.present = present;
        if constexpr (!live)
            e.sources = { TweakRegistry::Source{ static_cast<const void*>(&sources), sizeof(S) }... };
        if constexpr (present)
        {
            // Contiguous: present() uploads the range alone.
            assert((m_presentEnd == 0 || m_presentEnd == e.offset || e.offset - m_presentEnd < 16) && "UboBlock: the UboPresent values must be registered together");
            if (m_presentEnd == 0)
                m_presentBegin = e.offset;
            m_presentEnd = e.offset + e.size * oc::max(e.count, 1u);
        }
    }

    oc::vector<Entry> m_entries;
    oc::vector<uint8> m_bytes; // the block as uploaded
    uint32 m_end = 0;
    uint32 m_presentBegin = 0;
    uint32 m_presentEnd = 0;
};

namespace RendererVKLayout
{
    // The GLSL text of the whole declaration (UboDeclaration.cpp): the flat block (every entry at its offset), the
    // UBO_LIVE_ defines and the consts. baked[i] (parallel to the entries): entry i is a const of its value in
    // `bakedValues` (the block's bytes) instead of a block member (the member keeps its slot as u_<name>Baked).
    export oc::string buildUboDeclaration(const UboBlock& block, const uint8* bakedValues, oc::span<const uint8> baked);
    // False when a value is not finite (a const cannot spell it: the entry then stays live).
    export bool isUboValueFinite(EUboType type, const uint8* bytes);
}

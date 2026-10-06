export module RendererVK:UboBlock;

import Core;
import Core.glm;
import Settings.Tweaks;

import :Layout;

// THE FRAME UBO: one std140 block, laid out by registration - no C++ struct mirrors it. Two kinds of member:
//
// * ROOT values (bind): written by the frame build through a typed handle (UboValue<T> / UboArray<T>, an offset):
//   `block.set(h.fogLive.waveBand, v)` - the same cost as a struct write. Everything that depends on the camera, the
//   clock, the sun, the wind or a value the outside pushes per frame. Never baked. UboRoot.ixx (self-binding, UboGroup).
// * LOCKABLE values (add): a tweak variable as it is, or a lambda plus the settings variables it reads (its SOURCES,
//   by reference: their addresses, resolved by TweakRegistry::sourceState). evaluate() writes them every build. A
//   lockable value whose sources are all locked is a GLSL const instead (Renderer::applyUboLocks); an addLive one
//   never is (it also reads something that is no tweak). Renderer::registerUboFields.
//
// The GLSL type comes from the C++ type. The block packs the members with std140 rules in registration order; an
// array's element must be a 16-byte multiple (vec4, uvec4, mat4). buildUboDeclaration (UboDeclaration.cpp) makes the
// GLSL from the same entries: a flat block of u_<name>, each member at its offset.
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

    // A lockable value's stored type: bool and int travel as floats (the shaders compare them as floats).
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
        using T = std::remove_cvref_t<std::invoke_result_t<F&>>;
        explicit EvalFn(F f) : fn(oc::move(f)) {}
        void write(uint8* out) const override
        {
            const typename Stored<T>::type value = static_cast<typename Stored<T>::type>(fn());
            std::memcpy(out, &value, sizeof(value));
        }
        F fn;
    };
}

// A root value's handle: its offset in the block (UboBlock::bind).
export template<typename T>
struct UboValue
{
    uint32 offset = 0;
};

// A root array's handle (16-byte elements: std140 strides them by their size).
export template<typename T>
struct UboArray
{
    uint32 offset = 0;
    uint32 count = 0;
};

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
        oc::unique_ptr<UboBlockDetail::Eval> eval; // null: a root value (the build writes it)
        bool live = false;                     // never baked: a root value, or addLive
    };

    // ---- Root values: the build writes them through the handle -------------------------------------------------
    template<typename T>
    void bind(UboValue<T>& handle, const char* name)
    {
        handle.offset = push(name, UboBlockDetail::Layout<T>::type, UboBlockDetail::Layout<T>::size, UboBlockDetail::Layout<T>::align, 0);
    }
    template<typename T>
    void bind(UboArray<T>& handle, const char* name, uint32 count)
    {
        static_assert(UboBlockDetail::Layout<T>::size % 16 == 0, "std140 array elements: 16-byte multiples only");
        handle.offset = push(name, UboBlockDetail::Layout<T>::type, UboBlockDetail::Layout<T>::size, 16, count);
        handle.count = count;
    }

    // The handle alone fixes T: the value converts to it (a uint32 handle takes an int, a float one a double).
    template<typename T> void set(UboValue<T> handle, const std::type_identity_t<T>& value) { std::memcpy(m_bytes.data() + handle.offset, &value, sizeof(T)); }
    template<typename T> T get(UboValue<T> handle) const
    {
        T value;
        std::memcpy(&value, m_bytes.data() + handle.offset, sizeof(T));
        return value;
    }
    template<typename T> void set(UboArray<T> handle, uint32 i, const std::type_identity_t<T>& value)
    {
        assert(i < handle.count);
        std::memcpy(m_bytes.data() + handle.offset + i * sizeof(T), &value, sizeof(T));
    }
    template<typename T> T get(UboArray<T> handle, uint32 i) const
    {
        assert(i < handle.count);
        T value;
        std::memcpy(&value, m_bytes.data() + handle.offset + i * sizeof(T), sizeof(T));
        return value;
    }

    void zero(uint32 offset, uint32 size) { std::memset(m_bytes.data() + offset, 0, size); }

    // ---- Lockable values ---------------------------------------------------------------------------------------
    // A tweak variable as it is: also its own source.
    template<typename T> requires (!std::is_invocable_v<const T&>)
    void add(const char* name, const T& variable)
    {
        const T* p = &variable;
        addValue(name, [p] { return *p; }, { TweakRegistry::Source{ p, sizeof(T) } });
    }

    // A computed value and every variable it reads (none = a constant).
    template<typename F, typename... S> requires std::is_invocable_v<F&>
    void add(const char* name, F fn, const S&... sources)
    {
        addValue(name, oc::move(fn), { TweakRegistry::Source{ static_cast<const void*>(&sources), sizeof(S) }... });
    }

    // A value that is never baked.
    template<typename F> requires std::is_invocable_v<F&>
    void addLive(const char* name, F fn)
    {
        addValue(name, oc::move(fn), {});
        m_entries.back().live = true;
    }

    // Every lockable value into the block; the addLive ones only when asked (the init bake skips them: what they read
    // may not exist yet).
    void evaluate(bool includeLive = true)
    {
        for (const Entry& e : m_entries)
            if (e.eval && (includeLive || !e.live))
                e.eval->write(m_bytes.data() + e.offset);
    }

    const oc::vector<Entry>& entries() const { return m_entries; }
    const uint8* data() const { return m_bytes.data(); }
    uint32 size() const { return (uint32)m_bytes.size(); }

private:
    uint32 push(const char* name, RendererVKLayout::EUboType type, uint32 size, uint32 align, uint32 count)
    {
        for (const Entry& e : m_entries)
            assert(e.name != name && "UboBlock: a GLSL name registered twice");
        Entry& e = m_entries.emplace_back();
        e.name = name;
        e.type = type;
        e.offset = (m_end + align - 1u) & ~(align - 1u);
        e.size = size;
        e.count = count;
        e.live = true;
        m_end = e.offset + size * oc::max(count, 1u);
        // std140: the block's size rounds to 16; the next member after an array or a vec4 starts aligned anyway.
        m_bytes.resize((m_end + 15u) & ~15u, 0);
        return e.offset;
    }

    template<typename F>
    void addValue(const char* name, F fn, std::initializer_list<TweakRegistry::Source> sources)
    {
        using T = typename UboBlockDetail::Stored<std::remove_cvref_t<std::invoke_result_t<F&>>>::type;
        push(name, UboBlockDetail::Layout<T>::type, UboBlockDetail::Layout<T>::size, UboBlockDetail::Layout<T>::align, 0);
        Entry& e = m_entries.back();
        e.live = false;
        e.sources.assign(sources.begin(), sources.end());
        e.eval = oc::make_unique<UboBlockDetail::EvalFn<F>>(oc::move(fn));
    }

    oc::vector<Entry> m_entries;
    oc::vector<uint8> m_bytes; // the block as uploaded (persists across frames: the build reads last frame's mvps)
    uint32 m_end = 0;
};

// Binds root values in DECLARATION order: a handle struct's members initialize themselves from it, so the struct is
// the one list (UboRoot.ixx). `UboValue<float> enabled = g("enabled");` registers u_<prefix>enabled; an array passes
// its count.
export struct UboGroup
{
    UboBlock& block;
    const char* prefix;

    struct Binder
    {
        UboBlock& block;
        oc::string name;
        uint32 count;
        template<typename T> operator UboValue<T>() const
        {
            assert(count == 0 && "UboGroup: an array binds to a UboArray");
            UboValue<T> handle;
            block.bind(handle, name.c_str());
            return handle;
        }
        template<typename T> operator UboArray<T>() const
        {
            UboArray<T> handle;
            block.bind(handle, name.c_str(), count);
            return handle;
        }
    };
    Binder operator()(const char* name, uint32 count = 0) const { return Binder{ block, oc::string(prefix) + name, count }; }
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

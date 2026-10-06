export module RendererVK:UboFields;

import Core;
import Core.glm;
import Settings.Tweaks;

import :Layout;

// THE LOCKABLE FRAME UBO VALUES: one entry per GLSL member u_<name>, registered once (Renderer::registerUboFields, the
// end of RendererUbo.cpp). An entry is a tweak variable as it is, or a lambda computing the value plus the settings
// variables it reads (its SOURCES, by reference: their addresses, resolved by TweakRegistry::sourceState). The GLSL
// type comes from the value's C++ type. The list packs the entries with std140 rules in registration order into ONE
// block, which follows the root struct in the frame UBO (RendererVKLayout::UBO_FIELDS_OFFSET); evaluate() fills it
// every frame. An entry whose sources are all locked is a GLSL const instead (Renderer::applyUboLocks); an addLive
// entry never is (it also reads something that is no tweak: the render scale, the viewport, the loaded model).
//
// A lambda runs every frame, long after the registration returns: it captures `[&]` over local REFERENCES to
// long-lived objects only (a reference captured by reference names the object itself) or `this` - never a local value.

namespace UboFieldsDetail
{
    template<typename T> struct TypeOf;
    template<> struct TypeOf<float>     { static constexpr RendererVKLayout::EUboType type = RendererVKLayout::EUboType::Float; };
    template<> struct TypeOf<int>       { static constexpr RendererVKLayout::EUboType type = RendererVKLayout::EUboType::Float; };
    template<> struct TypeOf<bool>      { static constexpr RendererVKLayout::EUboType type = RendererVKLayout::EUboType::Float; };
    template<> struct TypeOf<uint32>    { static constexpr RendererVKLayout::EUboType type = RendererVKLayout::EUboType::Uint; };
    template<> struct TypeOf<glm::vec2> { static constexpr RendererVKLayout::EUboType type = RendererVKLayout::EUboType::Vec2; };
    template<> struct TypeOf<glm::vec3> { static constexpr RendererVKLayout::EUboType type = RendererVKLayout::EUboType::Vec3; };
    template<> struct TypeOf<glm::vec4> { static constexpr RendererVKLayout::EUboType type = RendererVKLayout::EUboType::Vec4; };

    // bool and int travel as floats (the shaders compare them as floats).
    inline void store(uint8* out, float v)            { std::memcpy(out, &v, 4); }
    inline void store(uint8* out, int v)              { store(out, float(v)); }
    inline void store(uint8* out, bool v)             { store(out, v ? 1.0f : 0.0f); }
    inline void store(uint8* out, uint32 v)           { std::memcpy(out, &v, 4); }
    inline void store(uint8* out, const glm::vec2& v) { std::memcpy(out, &v, 8); }
    inline void store(uint8* out, const glm::vec3& v) { std::memcpy(out, &v, 12); }
    inline void store(uint8* out, const glm::vec4& v) { std::memcpy(out, &v, 16); }

    struct Eval
    {
        virtual ~Eval() = default;
        virtual void write(uint8* out) const = 0;
    };
    template<typename F>
    struct EvalFn final : Eval
    {
        explicit EvalFn(F f) : fn(oc::move(f)) {}
        void write(uint8* out) const override { store(out, fn()); }
        F fn;
    };
}

export class UboFieldList
{
public:
    struct Entry
    {
        oc::string name;                       // the GLSL member is u_<name>
        RendererVKLayout::EUboType type;
        uint32 offset;                         // in the block
        uint32 size;
        oc::vector<TweakRegistry::Source> sources;
        oc::unique_ptr<UboFieldsDetail::Eval> eval;
        bool live = false;                     // addLive: never baked
    };

    // A tweak variable as it is: also its own source.
    template<typename T> requires (!std::is_invocable_v<const T&>)
    void add(const char* name, const T& variable)
    {
        const T* p = &variable;
        push(name, [p] { return *p; }, { TweakRegistry::Source{ p, sizeof(T) } });
    }

    // A computed value and every variable it reads (none = a constant).
    template<typename F, typename... S> requires std::is_invocable_v<F&>
    void add(const char* name, F fn, const S&... sources)
    {
        push(name, oc::move(fn), { TweakRegistry::Source{ static_cast<const void*>(&sources), sizeof(S) }... });
    }

    // A value that is never baked.
    template<typename F> requires std::is_invocable_v<F&>
    void addLive(const char* name, F fn)
    {
        push(name, oc::move(fn), {});
        m_entries.back().live = true;
    }

    // Every entry's value into the block (size() bytes); the live ones only when asked (the init bake skips them:
    // what they read may not exist yet).
    void evaluate(uint8* block, bool includeLive = true) const
    {
        for (const Entry& e : m_entries)
            if (includeLive || !e.live)
                e.eval->write(block + e.offset);
    }

    const oc::vector<Entry>& entries() const { return m_entries; }
    uint32 size() const { return (m_size + 15u) & ~15u; }

private:
    template<typename F>
    void push(const char* name, F fn, std::initializer_list<TweakRegistry::Source> sources)
    {
        using T = std::remove_cvref_t<std::invoke_result_t<F&>>;
        const RendererVKLayout::EUboType type = UboFieldsDetail::TypeOf<T>::type;
        const uint32 size = type == RendererVKLayout::EUboType::Vec2 ? 8u : type == RendererVKLayout::EUboType::Vec3 ? 12u : type == RendererVKLayout::EUboType::Vec4 ? 16u : 4u;
        const uint32 align = size == 12u ? 16u : size; // std140: a vec3 aligns like a vec4, the next scalar fills its tail
        for (const Entry& e : m_entries)
            assert(e.name != name && "UboFieldList: a GLSL name registered twice");
        Entry& e = m_entries.emplace_back();
        e.name = name;
        e.type = type;
        e.offset = (m_size + align - 1u) & ~(align - 1u);
        e.size = size;
        e.sources.assign(sources.begin(), sources.end());
        e.eval = oc::make_unique<UboFieldsDetail::EvalFn<F>>(oc::move(fn));
        m_size = e.offset + size;
    }

    oc::vector<Entry> m_entries;
    uint32 m_size = 0;
};

namespace RendererVKLayout
{
    // The GLSL text of the whole declaration (UboDeclaration.cpp): the struct-array types, then the flat block - the
    // root's members at their C++ offsets, the list's entries at UBO_FIELDS_OFFSET + theirs - the UBO_LIVE_ defines and
    // the consts. baked[i] (parallel to the entries): entry i is a const of its value in `bakedValues` (the block's
    // bytes) instead of a block member (the member keeps its slot as u_<name>Baked).
    export oc::string buildUboDeclaration(const UboFieldList& fields, const uint8* bakedValues, oc::span<const uint8> baked);
    // False when a value is not finite (a const cannot spell it: the entry then stays live).
    export bool isUboValueFinite(EUboType type, const uint8* bytes);
}

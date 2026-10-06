export module RendererVK:PushFields;

import Core;
import Core.glm;
import Settings.Tweaks;

import :Shader;

// THE LOCKABLE PUSH-CONSTANT VALUES: the frame UBO's lock pattern (UboBlock) for a push block. One list per group
// of shaders that read the same values at the same time (a pipeline, or the far-tree bake's passes). An entry is a
// push member pc_<name>, its value (a lambda) and the settings variables it is computed from (its SOURCES). The GLSL
// type comes from the value's C++ type, which must be the member's type in the push struct.
//
// The push block in the shader is instance-less with flat pc_ names, like the UBO's u_ names. A lockable member is
// declared through its generated define, and the consts follow the block:
//
//     layout (push_constant) uniform PC
//     {
//         uvec2 pc_size;            // runtime: written as it is
//         PC_DECL_vol_rMin;         // "float pc_vol_rMin" - or "float pc_vol_rMinBaked" while baked
//     };
//     PC_CONSTS                     // "const float pc_vol_rMin = 123.5;" for every baked entry
//
// A baked member keeps its slot (the C++ still pushes it; the shader no longer reads it), so the block layout never
// changes. PC_LIVE_<name> reads the member in both modes. The pipeline appends the list's defines (appendDefines) to
// every layout that compiles one of its shaders, and reloads them when update() reports a change.
//
// A lambda runs long after the registration returns: it captures `this` or references to long-lived objects only.

namespace PushFieldsDetail
{
    export enum class EType : uint8 { Float, Int, Uint, Vec2, Vec3, Vec4, Uvec2 };

    template<typename T> struct TypeOf;
    template<> struct TypeOf<float>      { static constexpr EType type = EType::Float; };
    template<> struct TypeOf<int32>      { static constexpr EType type = EType::Int; };
    template<> struct TypeOf<uint32>     { static constexpr EType type = EType::Uint; };
    template<> struct TypeOf<glm::vec2>  { static constexpr EType type = EType::Vec2; };
    template<> struct TypeOf<glm::vec3>  { static constexpr EType type = EType::Vec3; };
    template<> struct TypeOf<glm::vec4>  { static constexpr EType type = EType::Vec4; };
    template<> struct TypeOf<glm::uvec2> { static constexpr EType type = EType::Uvec2; };

    struct Eval
    {
        virtual ~Eval() = default;
        virtual void write(uint8* out) const = 0;
    };
    template<typename F>
    struct EvalFn final : Eval
    {
        explicit EvalFn(F f) : fn(oc::move(f)) {}
        void write(uint8* out) const override
        {
            const auto value = fn();
            std::memcpy(out, &value, sizeof(value));
        }
        F fn;
    };
}

export class PushFieldList
{
public:
    using EType = PushFieldsDetail::EType;

    struct Entry
    {
        oc::string name;                       // the GLSL member is pc_<name>
        EType type;
        uint32 size;
        oc::vector<TweakRegistry::Source> sources;
        oc::unique_ptr<PushFieldsDetail::Eval> eval;
        oc::array<uint8, 16> value{};          // the last evaluation
        oc::array<uint8, 16> bakedValue{};     // the const's value (while baked)
        bool locked = false;                   // every source locked (resolve)
        bool baked = false;
    };

    // A tweak variable as it is: also its own source.
    template<typename T> requires (!std::is_invocable_v<const T&>)
    void add(const char* name, const T& variable)
    {
        const T* p = &variable;
        push(name, [p] { return *p; }, { TweakRegistry::Source{ p, sizeof(T) } });
    }

    // A computed value and every variable it reads.
    template<typename F, typename... S> requires std::is_invocable_v<F&>
    void add(const char* name, F fn, const S&... sources)
    {
        push(name, oc::move(fn), { TweakRegistry::Source{ static_cast<const void*>(&sources), sizeof(S) }... });
    }

    // A lock click (resolve) or a changed setting: the next update() bakes again.
    void markDirty(bool resolve) { m_dirty = true; m_resolveDirty |= resolve; }
    bool isDirty() const { return m_dirty; }

    // Re-resolves the locks when asked, evaluates every entry and bakes the locked ones. Returns whether a const
    // appeared, went or changed its value (the owner then reloads the shaders that read the list). PushFields.cpp.
    bool update();

    // The PC_DECL_ / PC_LIVE_ defines and PC_CONSTS, from the last update().
    void appendDefines(oc::vector<ShaderDefine>& defines) const;

    const oc::vector<Entry>& entries() const { return m_entries; }

private:
    template<typename F>
    void push(const char* name, F fn, std::initializer_list<TweakRegistry::Source> sources)
    {
        using T = std::remove_cvref_t<std::invoke_result_t<F&>>;
        for (const Entry& e : m_entries)
            assert(e.name != name && "PushFieldList: a GLSL name registered twice");
        Entry& e = m_entries.emplace_back();
        e.name = name;
        e.type = PushFieldsDetail::TypeOf<T>::type;
        e.size = (uint32)sizeof(T);
        e.sources.assign(sources.begin(), sources.end());
        e.eval = oc::make_unique<PushFieldsDetail::EvalFn<F>>(oc::move(fn));
        m_dirty = m_resolveDirty = true;
    }

    void resolve();

    oc::vector<Entry> m_entries;
    bool m_dirty = true;
    bool m_resolveDirty = true;
};

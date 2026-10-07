export module RendererVK:PushBlock;

import Core;
import Core.glm;
import Settings.Tweaks;

// A PUSH BLOCK, the frame UBO's pattern (UboBlock) for push constants: its layout is REGISTERED, no C++ struct mirrors
// it. A handle struct per block (PushValue<T> members, each binding itself where it is declared through a PushGroup)
// IS the layout; the pipeline's layouts carry declaration() as their pushDeclaration, which the shader includes as
// "push.generated.glsl" - an instance-less block of pc_<name> members at explicit offsets (scalar layout), so a value
// can be a const while its neighbours stay members:
//
//     layout (push_constant, scalar) uniform PC
//     {
//         layout(offset = 0) uvec2 pc_size;
//         layout(offset = 8) float pc_vol_rMaxBaked;  // baked: the slot stays, the C++ still writes it
//     };
//     const float pc_vol_rMax = 4000.0;
//
// A recorded dispatch fills a PushData (a stack buffer) through the handles and pushes its bytes.
//
// LOCKABLE values (lockable): a handle that also gets a lambda + the settings variables it reads (its SOURCES). Its value
// is written by the PushData itself (writeLockables), so the fill site never computes it a second time; it is a GLSL
// const while every source is locked (update, then the owner reloads the block's pipelines). PC_LIVE_<name> reads the
// member in both modes. A lambda captures `this` or references to long-lived objects only.

namespace PushBlockDetail
{
    export enum class EType : uint8 { Float, Int, Uint, Vec2, Vec3, Vec4, Ivec2, Uvec2, Mat4, Address };

    // Scalar layout: every type aligns to its component (an address to 8).
    template<typename T> struct Layout;
    template<> struct Layout<float>      { static constexpr EType type = EType::Float; static constexpr uint32 align = 4; };
    template<> struct Layout<int32>      { static constexpr EType type = EType::Int;   static constexpr uint32 align = 4; };
    template<> struct Layout<uint32>     { static constexpr EType type = EType::Uint;  static constexpr uint32 align = 4; };
    template<> struct Layout<glm::vec2>  { static constexpr EType type = EType::Vec2;  static constexpr uint32 align = 4; };
    template<> struct Layout<glm::vec3>  { static constexpr EType type = EType::Vec3;  static constexpr uint32 align = 4; };
    template<> struct Layout<glm::vec4>  { static constexpr EType type = EType::Vec4;  static constexpr uint32 align = 4; };
    template<> struct Layout<glm::ivec2> { static constexpr EType type = EType::Ivec2; static constexpr uint32 align = 4; };
    template<> struct Layout<glm::uvec2> { static constexpr EType type = EType::Uvec2; static constexpr uint32 align = 4; };
    template<> struct Layout<glm::mat4>  { static constexpr EType type = EType::Mat4;  static constexpr uint32 align = 4; };
    template<> struct Layout<uint64>     { static constexpr EType type = EType::Address; static constexpr uint32 align = 8; };

    struct Eval
    {
        virtual ~Eval() = default;
        virtual void write(uint8* out) const = 0;
    };
    template<typename T, typename F>
    struct EvalFn final : Eval
    {
        explicit EvalFn(F f) : fn(oc::move(f)) {}
        void write(uint8* out) const override
        {
            const T value = static_cast<T>(fn());
            std::memcpy(out, &value, sizeof(T));
        }
        F fn;
    };
}

// A push value's handle: its offset in the block.
export template<typename T>
struct PushValue
{
    uint32 offset = 0;
};

export class PushBlock
{
public:
    static constexpr uint32 MAX_SIZE = 128; // the guaranteed push-constant minimum

    struct Entry
    {
        oc::string name;                        // the GLSL member is pc_<name>
        oc::string glslType;
        PushBlockDetail::EType type;
        uint32 offset;
        uint32 size;
        // Lockable only (eval set):
        oc::vector<TweakRegistry::Source> sources;
        oc::unique_ptr<PushBlockDetail::Eval> eval;
        oc::array<uint8, 16> value{};           // the last evaluation
        oc::array<uint8, 16> bakedValue{};      // the const's value (while baked)
        bool locked = false;                    // every source locked (resolve)
        bool baked = false;
    };

    // glslType: the member's GLSL type where the C++ type cannot say it (a buffer reference's type for a uint64
    // address); null = from T.
    template<typename T>
    void bind(PushValue<T>& handle, const char* name, const char* glslType = nullptr)
    {
        static_assert(sizeof(T) % 4 == 0);
        for (const Entry& e : m_entries)
            assert(e.name != name && "PushBlock: a GLSL name registered twice");
        assert((glslType != nullptr || PushBlockDetail::Layout<T>::type != PushBlockDetail::EType::Address) && "PushBlock: an address names its GLSL type");
        const uint32 align = PushBlockDetail::Layout<T>::align;
        Entry& e = m_entries.emplace_back();
        e.name = name;
        e.type = PushBlockDetail::Layout<T>::type;
        e.glslType = glslType ? glslType : typeName(e.type);
        e.offset = (m_size + align - 1u) & ~(align - 1u);
        e.size = (uint32)sizeof(T);
        m_size = e.offset + e.size;
        assert(m_size <= MAX_SIZE && "PushBlock: over the 128-byte push-constant minimum");
        handle.offset = e.offset;
    }

    // Makes a bound value LOCKABLE: its value from fn, its sources the settings variables fn reads.
    template<typename T, typename F, typename... S> requires std::is_invocable_v<F&>
    void lockable(PushValue<T> handle, F fn, const S&... sources)
    {
        static_assert(sizeof(T) <= 16, "PushBlock: a lockable value is a scalar or a vector");
        Entry& e = entryAt(handle.offset);
        e.sources = { TweakRegistry::Source{ static_cast<const void*>(&sources), sizeof(S) }... };
        e.eval = oc::make_unique<PushBlockDetail::EvalFn<T, F>>(oc::move(fn));
        m_dirty = m_resolveDirty = true;
    }
    // A tweak variable as it is (also its own source).
    template<typename T>
    void lockable(PushValue<T> handle, const T& variable)
    {
        const T* p = &variable;
        lockable(handle, [p] { return *p; }, variable);
    }

    // A lock click (resolve) or a changed setting: the next update() bakes again.
    void markDirty(bool resolve) { m_dirty = true; m_resolveDirty |= resolve; }
    bool isDirty() const { return m_dirty; }
    // Re-resolves the locks when asked, evaluates the lockable values and bakes the locked ones. True = a const
    // appeared, went or changed: the owner reloads the block's pipelines (their layouts take declaration()). PushBlock.cpp.
    bool update();

    // The GLSL "push.generated.glsl" serves: the block, the PC_LIVE_ defines, the consts (as of the last update()).
    oc::string declaration() const;
    void writeLockables(uint8* bytes) const
    {
        for (const Entry& e : m_entries)
            if (e.eval)
                e.eval->write(bytes + e.offset);
    }

    const oc::vector<Entry>& entries() const { return m_entries; }
    uint32 size() const { return (m_size + 3u) & ~3u; }

private:
    static const char* typeName(PushBlockDetail::EType type);
    Entry& entryAt(uint32 offset)
    {
        for (Entry& e : m_entries)
            if (e.offset == offset)
                return e;
        assert(false && "PushBlock: no value at that offset");
        return m_entries.front();
    }
    void resolve();

    oc::vector<Entry> m_entries;
    uint32 m_size = 0;
    bool m_dirty = true;
    bool m_resolveDirty = true;
};

// One dispatch's push bytes: the lockable values written at construction, the rest through the handles.
export class PushData
{
public:
    explicit PushData(const PushBlock& block) : m_size(block.size()) { block.writeLockables(m_bytes.data()); }
    template<typename T> void set(PushValue<T> handle, const std::type_identity_t<T>& value) { std::memcpy(m_bytes.data() + handle.offset, &value, sizeof(T)); }
    const void* data() const { return m_bytes.data(); }
    uint32 size() const { return m_size; }

private:
    oc::array<uint8, PushBlock::MAX_SIZE> m_bytes{};
    uint32 m_size;
};

// Binds push values in DECLARATION order (UboGroup's pattern): `PushValue<float> radius = g("radius");`, an address
// `PushValue<uint64> pieces = g("pieces", "PieceList");`.
export struct PushGroup
{
    PushBlock& block;
    const char* prefix = "";

    struct Binder
    {
        PushBlock& block;
        oc::string name;
        const char* glslType;
        template<typename T> operator PushValue<T>() const
        {
            PushValue<T> handle;
            block.bind(handle, name.c_str(), glslType);
            return handle;
        }
    };
    Binder operator()(const char* name, const char* glslType = nullptr) const { return Binder{ block, oc::string(prefix) + name, glslType }; }
};

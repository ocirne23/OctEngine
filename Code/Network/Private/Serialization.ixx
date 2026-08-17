export module Network:Serialization;

import Core;

// Minimal byte-level serialization over a caller-provided buffer. The wire format is raw native
// little-endian bytes: no tags, no versioning, no padding — both ends must agree on the layout.
// Overflow never touches memory out of bounds; check overflowed() after a batch of operations.

export class NetWriter final
{
public:

    explicit NetWriter(oc::span<uint8> buffer) : m_buffer(buffer) {}

    template<typename T> requires std::is_trivially_copyable_v<T>
    void write(const T& value)
    {
        if (uint8* dst = reserve(sizeof(T)))
            memcpy(dst, &value, sizeof(T));
    }

    // patch a value written earlier (e.g. a count written before its items)
    template<typename T> requires std::is_trivially_copyable_v<T>
    void writeAt(size_t offset, const T& value)
    {
        assert(offset + sizeof(T) <= m_pos);
        if (offset + sizeof(T) <= m_pos)
            memcpy(m_buffer.data() + offset, &value, sizeof(T));
    }

    void writeBytes(oc::span<const uint8> bytes)
    {
        if (uint8* dst = reserve(bytes.size()); dst && !bytes.empty())
            memcpy(dst, bytes.data(), bytes.size());
    }

    void writeString(oc::string_view str)
    {
        writeVarUInt(str.size());
        if (uint8* dst = reserve(str.size()); dst && !str.empty())
            memcpy(dst, str.data(), str.size());
    }

    // LEB128: 1 byte below 128, 2 bytes below 16384, ...
    void writeVarUInt(uint64 value)
    {
        while (value >= 0x80)
        {
            write<uint8>(uint8(value) | 0x80);
            value >>= 7;
        }
        write<uint8>(uint8(value));
    }
    void writeVarInt(int64 value) { writeVarUInt((uint64(value) << 1) ^ uint64(value >> 63)); } // zigzag

    // float quantized over [min, max] to the full range of UInt
    template<typename UInt> requires (std::is_unsigned_v<UInt> && sizeof(UInt) <= 4)
    void writeQuantized(float value, float min, float max)
    {
        constexpr float maxVal = float((uint64(1) << (sizeof(UInt) * 8)) - 1);
        float t = (value - min) / (max - min);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        write<UInt>(UInt(t * maxVal + 0.5f));
    }
    void writeUnorm8(float v)  { writeQuantized<uint8>(v, 0.0f, 1.0f); }
    void writeUnorm16(float v) { writeQuantized<uint16>(v, 0.0f, 1.0f); }
    void writeSnorm8(float v)  { writeQuantized<uint8>(v, -1.0f, 1.0f); }
    void writeSnorm16(float v) { writeQuantized<uint16>(v, -1.0f, 1.0f); }

    // advance the cursor and return the destination, null (+overflow) if it doesn't fit
    uint8* reserve(size_t numBytes)
    {
        if (numBytes > m_buffer.size() - m_pos) // overflow-safe, see NetReader::consume
        {
            m_overflowed = true;
            return nullptr;
        }
        uint8* ptr = m_buffer.data() + m_pos;
        m_pos += numBytes;
        return ptr;
    }

    oc::span<const uint8> data() const { return m_buffer.first(m_pos); }
    size_t size() const { return m_pos; }
    size_t capacity() const { return m_buffer.size(); }
    bool overflowed() const { return m_overflowed; }
    void reset() { m_pos = 0; m_overflowed = false; }

private:

    oc::span<uint8> m_buffer;
    size_t m_pos = 0;
    bool m_overflowed = false;
};

export class NetReader final
{
public:

    explicit NetReader(oc::span<const uint8> data) : m_data(data) {}

    template<typename T> requires std::is_trivially_copyable_v<T>
    T read()
    {
        T value{};
        if (const uint8* src = consume(sizeof(T)))
            memcpy(&value, src, sizeof(T));
        return value;
    }

    // zero-copy view into the buffer, empty (+overflow) when not enough bytes remain
    oc::span<const uint8> readBytes(size_t numBytes)
    {
        const uint8* src = consume(numBytes);
        return src ? oc::span<const uint8>(src, numBytes) : oc::span<const uint8>();
    }

    // view into the buffer — only valid while the underlying buffer lives
    oc::string_view readString()
    {
        const size_t size = size_t(readVarUInt());
        const uint8* src = consume(size);
        return src ? oc::string_view(reinterpret_cast<const char*>(src), size) : oc::string_view();
    }

    uint64 readVarUInt()
    {
        uint64 value = 0;
        for (uint32 shift = 0; shift < 64; shift += 7)
        {
            const uint8 byte = read<uint8>();
            value |= uint64(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0)
                break;
        }
        return value;
    }
    int64 readVarInt()
    {
        const uint64 v = readVarUInt();
        return int64(v >> 1) ^ -int64(v & 1);
    }

    template<typename UInt> requires (std::is_unsigned_v<UInt> && sizeof(UInt) <= 4)
    float readQuantized(float min, float max)
    {
        constexpr float maxVal = float((uint64(1) << (sizeof(UInt) * 8)) - 1);
        return min + (max - min) * (float(read<UInt>()) / maxVal);
    }
    float readUnorm8()  { return readQuantized<uint8>(0.0f, 1.0f); }
    float readUnorm16() { return readQuantized<uint16>(0.0f, 1.0f); }
    float readSnorm8()  { return readQuantized<uint8>(-1.0f, 1.0f); }
    float readSnorm16() { return readQuantized<uint16>(-1.0f, 1.0f); }

    void skip(size_t numBytes) { consume(numBytes); }
    size_t position() const { return m_pos; }
    size_t remaining() const { return m_data.size() - m_pos; }
    bool atEnd() const { return m_pos >= m_data.size(); }
    bool overflowed() const { return m_overflowed; }

private:

    const uint8* consume(size_t numBytes)
    {
        // subtraction, not `m_pos + numBytes > size()`: numBytes is a wire-supplied length and near
        // 2^64 that addition wraps past the check
        if (numBytes > m_data.size() - m_pos)
        {
            m_overflowed = true;
            return nullptr;
        }
        const uint8* ptr = m_data.data() + m_pos;
        m_pos += numBytes;
        return ptr;
    }

    oc::span<const uint8> m_data;
    size_t m_pos = 0;
    bool m_overflowed = false;
};

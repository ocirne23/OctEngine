export module Core.BitPathTrie;

import Core;

// Bitmap path trie: a static (build once, read many) trie for file paths, laid out to be baked offline and memory
// mapped. BitmapPathTrie builds the storage; BitmapPathTrieView does lookups and iteration on it, whether the storage is
// owned by a BitmapPathTrie or memory mapped.
//
// - Paths use a 64 symbol alphabet (6 bits per symbol). The trie is case insensitive (uppercase folds to the lowercase
//   symbol); the original case is stored separately, so canonical paths can be rebuilt. Characters outside the
//   alphabet are refused on insert and never match on lookup.
// - Every node is 16 bytes. An internal node stores a 64 bit mask with one bit per symbol that has a child. The
//   children of a node are contiguous, so the child for symbol s is firstChild + popcnt(mask & ((1 << s) - 1)).
// - Bit 0 of the mask (the end symbol) marks that a key ends at this node. As the lowest bit, that child is always the
//   first child. This allows keys that are a prefix of other keys (a.txt / a.txt.bin).
// - Every node stores up to 5 symbols of path compression inline. The branch symbol is implicit in the child's
//   position and is not stored, only its case is (in the child).
// - Leaves reuse the mask for 9 more inline symbols (14 in total). Longer endings go to the symbol pool.
// - Subtrees with at most maxBucketKeys keys (see build()) are stored as a bucket instead of nodes: a sorted list of
//   front coded entries in the symbol pool, their keys bit packed in a separate array. Every entry is one header
//   symbol (symbols to drop from the end of the previous entry + number of new symbols, the split chosen per trie to
//   minimize size) followed by the new symbols. Lookups scan the bucket linearly, iteration decodes it in order.
// - The symbol pool's case stream is only stored when at least one path has uppercase characters.
// - Lookups compare up to 5 (label) or 9 (leaf / tail) symbols at once by xor-ing packed symbol words.
// - Nodes and bucket entries shared by several keys store one case (from the first key in symbol order). Keys whose
//   case differs get a list of positions to flip in a sparse case exception table.

export namespace oc
{
    namespace pathTrieDetail
    {
        inline constexpr uint32 SymbolBits = 6; // 64 possible characters in paths
        inline constexpr uint32 MaxPath = 1024;
        inline constexpr uint32 MaxChildOrKey = (1u << 26) - 1; // Node::childOrKey

        struct Node;
        struct Query;
    }

    // Keys whose case differs from the case stored in the shared nodes. Sorted by key; an entry's positions are
    // [firstPosition, next.firstPosition), and the last entry is a sentinel.
    struct BitmapPathTrieCaseException
    {
        uint32 key = UINT32_MAX;
        uint32 firstPosition = 0;
    };

    // All arrays of a baked trie; they can point into a memory mapped file.
    struct BitmapPathTrieStorage
    {
        oc::span<const pathTrieDetail::Node> nodes;
        oc::span<const uint8> poolSymbols;                          // 6 bit symbol stream (tails and buckets) + padding
        oc::span<const uint8> poolCase;                             // 1 uppercase bit per pool symbol + padding, empty when no path has uppercase
        oc::span<const uint8> bucketKeys;                           // bucketKeyBits per key + padding
        oc::span<const BitmapPathTrieCaseException> caseExceptions; // empty when paths sharing a prefix also share its case
        oc::span<const uint16> casePositions;                       // character positions to flip to the other case
        uint32 bucketKeyBits = 0;
        uint32 bucketDropBits = 0;                                  // bits of a bucket entry header used for the drop count
    };

    // Node breakdown of a trie, see BitmapPathTrieView::stats().
    struct BitmapPathTrieStats
    {
        size_t branchNodes = 0;         // internal nodes with 2 or more children
        size_t chainNodes = 0;          // internal nodes with exactly 1 child
        size_t bucketNodes = 0;
        size_t bucketKeys = 0;
        size_t bucketSymbols = 0;       // pool symbols used by buckets (headers and new symbols)
        size_t bucketHeaderSymbols = 0; // pool symbols used by bucket entry headers, escaped counts included
        size_t leafNodes = 0;
        size_t endLeafNodes = 0;        // leaves of keys that end where other keys continue (end symbol children)
        size_t tailLeafNodes = 0;       // leaves with their ending in the symbol pool
        size_t tailSymbols = 0;
        size_t inlineLeafSymbols = 0;   // symbols stored inline in leaves without a tail (max 14 per node)
        size_t caseExceptionKeys = 0;   // keys whose case differs from the case stored in the trie
        size_t casePositions = 0;       // character positions toggled by those case exceptions
        size_t keyDepthSum = 0;         // sum over all keys of the nodes a lookup visits
        size_t maxKeyDepth = 0;
        size_t branchChildCounts[7] = {}; // children per branch node: [1] 2, [2] 3-4, [3] 5-8, [4] 9-16, [5] 17-32, [6] 33-64
    };

    // Entry passed to the iteration callbacks. path is the full canonical path, valid only during the callback.
    struct BitmapPathTrieEntry
    {
        oc::string_view path;
        uint32 key = UINT32_MAX; // UINT32_MAX for directories
        bool isDirectory = false;
    };

    // Read only view on a baked trie; the storage must outlive the view.
    class BitmapPathTrieView final
    {
        friend class BitmapPathTrie;

    public:

        using Node = pathTrieDetail::Node;
        static constexpr uint32 InvalidKey = UINT32_MAX;

        BitmapPathTrieView() = default;
        explicit BitmapPathTrieView(const BitmapPathTrieStorage& source);

        bool empty() const;
        size_t nodeCount() const;
        size_t memoryUsage() const;
        const BitmapPathTrieStorage& storage() const;

        // The key of path, or InvalidKey. outCanonical (same size as path) receives the path in its original case.
        uint32 find(oc::string_view path, bool caseSensitive = false, oc::span<char> outCanonical = {}) const;
        bool contains(oc::string_view path, bool caseSensitive = false) const;

        // func(const BitmapPathTrieEntry&) for every key, in symbol order.
        template<typename Func>
        void forEach(Func func) const;

        // func(const BitmapPathTrieEntry&) for every key starting with prefix (case insensitive, typically a directory
        // ending in '/'). Non recursive skips the keys in sub directories and reports every sub directory once instead,
        // as an entry with isDirectory set.
        template<typename Func>
        void forEachChild(oc::string_view prefix, bool recursive, Func func) const;

        // Walks the whole trie and breaks its nodes down, for evaluating the layout.
        BitmapPathTrieStats stats() const;

    private:

        struct IterationContext
        {
            oc::string_view prefix;
            bool recursive = false;
            char path[pathTrieDetail::MaxPath] = {};
        };

        template<bool Output>
        uint32 findKey(oc::string_view path, char* out) const;
        template<bool Output>
        bool matchLeaf(const Node& node, const pathTrieDetail::Query& query, uint32 pos, uint32 length, char* out) const;
        template<bool Output>
        uint32 findInBucket(const Node& node, const pathTrieDetail::Query& query, uint32 pos, char* out) const;
        void applyCaseExceptions(uint32 key, char* path) const;
        uint32 poolSymbol(uint32 index) const;
        bool isPoolUpper(uint32 index) const;
        uint32 readBucketCount(uint32& index) const;
        void readBucketEntryHeader(uint32& index, uint32 previousLength, uint32& outShared, uint32& outAdded) const;
        uint32 bucketKey(uint32 index) const;

        template<typename Func>
        void visit(uint32 nodeIndex, uint32 length, IterationContext& context, Func& func) const;
        template<typename Func>
        void visitBucket(const Node& node, uint32 length, IterationContext& context, Func& func) const;
        template<typename Func>
        bool appendSymbol(uint32 symbol, bool upper, uint32& length, IterationContext& context, Func& func) const;
        template<typename Func>
        void reportKey(uint32 key, uint32 length, IterationContext& context, Func& func) const;

        void collectStats(uint32 nodeIndex, size_t depth, bool isEndChild, BitmapPathTrieStats& out) const;

        BitmapPathTrieStorage m_storage;
    };

    // Collects paths and bakes them into a BitmapPathTrieStorage.
    class BitmapPathTrie final
    {
    public:

        using Node = pathTrieDetail::Node;
        static constexpr uint32 InvalidKey = BitmapPathTrieView::InvalidKey;
        static constexpr uint32 MaxKey = pathTrieDetail::MaxChildOrKey;
        static constexpr uint32 DefaultMaxBucketKeys = 64; // subtrees with at most this many keys become a bucket instead of nodes

        // Queues a path. False if it has characters outside the alphabet, is too long, or key > MaxKey.
        // Duplicate paths (case insensitive) are caught by build().
        bool insert(oc::string_view path, uint32 key);
        // (Re)builds the storage from every inserted path. False (and an empty trie) if a path was inserted twice.
        // Subtrees with 2 to maxBucketKeys keys become a bucket; 0 or 1 disables buckets.
        bool build(uint32 maxBucketKeys = DefaultMaxBucketKeys);

        void clear();
        void clearPending(); // releases the inserted paths; build() cannot run after this

        BitmapPathTrieStorage storage() const;
        BitmapPathTrieView view() const;

    private:

        static constexpr uint8 PendingUpperFlag = 1u << pathTrieDetail::SymbolBits;

        struct PendingEntry
        {
            uint32 symbolOffset = 0;
            uint32 symbolCount = 0;
            uint32 key = InvalidKey;
        };

        struct PendingBucket
        {
            uint32 nodeIndex = 0;
            uint32 begin = 0;      // range in m_pending
            uint32 end = 0;
            uint32 entryDepth = 0; // symbols covered by the path to the bucket and its label
        };

        uint32 symbolAt(const PendingEntry& entry, uint32 pos) const;
        bool isUpper(const PendingEntry& entry, uint32 pos) const;
        void packLabel(const PendingEntry& entry, uint32 pos, uint32 count, Node& node) const;
        uint32 allocateNodes(uint32 count);
        uint32 sharedSymbolCount(uint32 begin, uint32 end, uint32 depth) const;
        void buildNode(uint32 nodeIndex, uint32 begin, uint32 end, uint32 depth);
        void buildBucket(uint32 nodeIndex, uint32 begin, uint32 end, uint32 depth);
        void buildLeaf(uint32 nodeIndex, const PendingEntry& entry, uint32 depth);
        template<typename Func>
        void forEachBucketEntry(const PendingBucket& bucket, Func func) const;
        static uint32 countSymbols(uint32 count);
        void appendPoolSymbol(uint32 symbol, bool upper);
        void appendPoolCount(uint32 count);
        void writeBuckets();
        void packBucketKeys();
        void buildCaseExceptions();

        oc::vector<uint8> m_pendingSymbols;                         // symbols of all inserted paths back to back, symbol | PendingUpperFlag
        oc::vector<PendingEntry> m_pending;                         // one entry per inserted path, sorted by build()
        oc::vector<PendingBucket> m_pendingBuckets;                 // buckets whose entries writeBuckets() writes
        oc::vector<uint32> m_pendingBucketKeys;                     // bucket keys in bucket order, packed at the end of build()
        oc::vector<Node> m_nodes;                                   // the trie: root at index 0, children of a node contiguous
        oc::vector<uint8> m_poolSymbols;                            // 6 bit symbol stream holding leaf tails and bucket entries
        oc::vector<uint8> m_poolCase;                               // 1 uppercase bit per pool symbol, empty when no path has uppercase
        oc::vector<uint8> m_bucketKeys;                             // bucket keys, bit packed with m_bucketKeyBits each
        oc::vector<BitmapPathTrieCaseException> m_caseExceptions;   // keys whose case differs from the trie, sorted by key + sentinel
        oc::vector<uint16> m_casePositions;                         // character positions to toggle per case exception
        uint32 m_poolSymbolCount = 0;
        uint32 m_bucketKeyBits = 0;
        uint32 m_bucketDropBits = 0;
        uint32 m_maxBucketKeys = 0;
        bool m_hasUpper = false;                                    // any inserted path has uppercase; otherwise no case stream is written
    };

    // ---- Implementation: pathTrieDetail ----------------------------------------------------------------------------

    namespace pathTrieDetail
    {
        inline constexpr uint32 SymbolMask = (1u << SymbolBits) - 1;
        inline constexpr uint32 AlphabetSize = 1u << SymbolBits;
        inline constexpr uint8 InvalidSymbol = 0xFF;            // character outside the alphabet
        inline constexpr uint32 StreamPadding = sizeof(uint64); // zero bytes after every bit stream, so a 64 bit load at any used byte stays in bounds
        inline constexpr uint32 SymbolsPerLoad = (64 - 7) / SymbolBits; // symbols a loadBits() result always holds whole

        // A bucket entry starts with a header symbol of two fields: the symbols to drop from the end of the previous entry
        // (the low bucketDropBits bits) and the number of new symbols that follow (the rest). A field with all bits set is
        // an escape: the value follows as an explicit count, one symbol below CountEscape, otherwise the escape and two
        // symbols (12 bits).
        inline constexpr uint32 CountEscape = SymbolMask;
        inline constexpr uint32 MaxCount = (1u << (2 * SymbolBits)) - 1;
        inline constexpr uint32 MinDropBits = 1;
        inline constexpr uint32 MaxDropBits = SymbolBits - 1;
        static_assert(MaxPath <= MaxCount);

        // symbol -> character. Symbol 0 is the end symbol: it ends a key and pads labels and streams. 62 and 63 are unused.
        inline constexpr char g_symbolToChar[AlphabetSize] =
        {
            '\0',
            'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
            '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
            '.', '/', '_', '-', ' ',
            '(', ')', '[', ']', '{', '}', '+', '&', '~', '$', '#', '@', '!', '\'', ',', ';', '=', '%', '^', '`',
            '\0', '\0',
        };

        // character -> symbol, uppercase folds to the lowercase symbol
        struct CharTable
        {
            uint8 symbols[256] = {};

            constexpr CharTable()
            {
                for (uint8& symbol : symbols)
                    symbol = InvalidSymbol;

                for (uint32 symbol = 1; symbol < AlphabetSize; ++symbol)
                    if (g_symbolToChar[symbol] != '\0')
                        symbols[static_cast<uint8>(g_symbolToChar[symbol])] = static_cast<uint8>(symbol);

                for (char c = 'A'; c <= 'Z'; ++c)
                    symbols[static_cast<uint8>(c)] = symbols[static_cast<uint8>(c - 'A' + 'a')];
            }
        };
        inline constexpr CharTable g_charTable{};

        inline uint32 charToSymbol(char c)
        {
            return g_charTable.symbols[static_cast<uint8>(c)];
        }

        inline bool isUpperChar(char c)
        {
            return static_cast<uint32>(static_cast<uint8>(c)) - 'A' < 26u;
        }

        inline char symbolToChar(uint32 symbol, bool upper)
        {
            const char c = g_symbolToChar[symbol];
            return upper ? static_cast<char>(c - 'a' + 'A') : c;
        }

        // 64 bits starting at bit `bit`, as a little endian word. Every stream ends in StreamPadding bytes, so the load is
        // always in bounds; the result has at least 57 valid bits (9 symbols).
        inline uint64 loadBits(const uint8* stream, size_t bit)
        {
            uint64 word = 0;
            memcpy(&word, stream + (bit >> 3), sizeof(word));
            return word >> (bit & 7);
        }

        // Symbols are never 0 and are packed from the lowest bits, so the highest set bit gives the count.
        inline uint32 packedCount(uint64 packed)
        {
            return (oc::bitWidth(packed) + SymbolBits - 1) / SymbolBits;
        }

        // Mask over count packed symbols, count <= SymbolsPerLoad.
        inline uint64 packedMask(uint32 count)
        {
            return (1ull << (count * SymbolBits)) - 1;
        }

        // Writes count packed symbols as characters; bit i of upperMask makes symbol i uppercase.
        inline void writeSymbols(char* out, uint64 packed, uint32 count, uint64 upperMask)
        {
            for (uint32 i = 0; i < count; ++i)
            {
                const uint32 symbol = static_cast<uint32>(packed >> (i * SymbolBits)) & SymbolMask;
                out[i] = symbolToChar(symbol, ((upperMask >> i) & 1) != 0);
            }
        }

        // Baked to disk, so the layout must match on every compiler: each bit field group uses one underlying type and
        // fills its word exactly, which MSVC and clang both allocate from the lowest bit up.
        struct Node
        {
            static constexpr uint32 LabelSymbols = 5;

            // which union member is live, stored outside the union in `kind`
            enum class ENodeKind : uint32
            {
                Internal = 0,   // childMask
                Bucket = 1,     // bucket
                InlineLeaf = 2, // inlineLeaf
                TailLeaf = 3,   // tailLeaf
            };

            struct Bucket
            {
                static constexpr uint32 MaxKeys = (1u << 7) - 1; // keyCount

                uint64 poolOffset : 32; // symbol pool offset of the first entry
                uint64 keyCount : 7;
                uint64 unused : 25;
            };
            static_assert(sizeof(Bucket) == 8);

            struct InlineLeaf
            {
                static constexpr uint32 MaxSymbols = 9; // symbols / upper

                uint64 symbols : 54; // up to 9 more symbols (0 = padding)
                uint64 upper : 9;    // their uppercase mask
                uint64 unused : 1;
            };
            static_assert(sizeof(InlineLeaf) == 8);

            struct TailLeaf
            {
                static constexpr uint32 MaxLength = (1u << 16) - 1; // length

                uint64 poolOffset : 32; // symbol pool offset of the tail
                uint64 length : 16;
                uint64 unused : 16;
            };
            static_assert(sizeof(TailLeaf) == 8);

            union
            {
                uint64 childMask = 0; // internal: bit s set = a child for symbol s; bit 0 = a key ends here (child 0)
                Bucket bucket;
                InlineLeaf inlineLeaf;
                TailLeaf tailLeaf;
            };

            uint32 labelUpper : 5 = 0; // uppercase mask of the label symbols

            // internal: index of the first child
            // bucket:   index of the first bucket key
            // leaf:     the key (inline and tail leaves, 67 million max)
            uint32 childOrKey : 26 = 0;
            uint32 branchUpper : 1 = 0; // the branch symbol leading to this node is uppercase

            uint32 label : 30 = 0; // up to 5 label symbols, 6 bits each (0 = padding)
            uint32 kind : 2 = 0;   // ENodeKind

            ENodeKind getKind() const;
            void setKind(ENodeKind newKind);
            bool isBucket() const;
            bool isLeaf() const;
            bool hasTail() const;
            bool hasChild(uint32 symbol) const;
            uint32 getChildIndex(uint32 symbol) const;
        };
        static_assert(sizeof(Node) == 16);
        static_assert(Node::TailLeaf::MaxLength >= MaxPath, "Tail length field too small for MaxPath");

        inline Node::ENodeKind Node::getKind() const { return static_cast<ENodeKind>(kind); }
        inline void Node::setKind(ENodeKind newKind) { kind = static_cast<uint32>(newKind); }
        inline bool Node::isBucket() const { return getKind() == ENodeKind::Bucket; }
        inline bool Node::isLeaf() const { return getKind() == ENodeKind::InlineLeaf || getKind() == ENodeKind::TailLeaf; }
        inline bool Node::hasTail() const { return getKind() == ENodeKind::TailLeaf; }
        inline bool Node::hasChild(uint32 symbol) const { return ((childMask >> symbol) & 1) != 0; }
        inline uint32 Node::getChildIndex(uint32 symbol) const
        {   // a 64 bit one: symbol 63 would overflow a 32 bit shift
            return childOrKey + oc::popcnt(childMask & ((1ull << symbol) - 1));
        }

        // A lookup path as a 6 bit symbol stream, zero padded so loads past the end read end symbols.
        struct Query
        {   // the symbol bytes of the longest path (up to the last partial byte), then the padding
            uint8 symbols[(MaxPath * SymbolBits + 7) / 8 + StreamPadding];
            bool encode(oc::string_view path);
            uint32 symbolAt(uint32 pos) const;
        };

        inline bool Query::encode(oc::string_view path)
        {
            uint8* out = symbols;
            uint64 acc = 0;
            uint32 accBits = 0;
            for (size_t i = 0; i < path.size(); ++i)
            {
                const uint32 symbol = charToSymbol(path[i]);
                if (symbol == InvalidSymbol)
                    return false;

                acc |= uint64(symbol) << accBits;
                accBits += SymbolBits;
                while (accBits >= 8)
                {
                    *out++ = static_cast<uint8>(acc);
                    acc >>= 8;
                    accBits -= 8;
                }
            }
            *out++ = static_cast<uint8>(acc);
            memset(out, 0, StreamPadding);
            return true;
        }

        inline uint32 Query::symbolAt(uint32 pos) const
        {
            return static_cast<uint32>(loadBits(symbols, size_t(pos) * SymbolBits)) & SymbolMask;
        }
    }

    // ---- Implementation: BitmapPathTrieView ------------------------------------------------------------------------

    inline BitmapPathTrieView::BitmapPathTrieView(const BitmapPathTrieStorage& source)
        : m_storage(source)
    {
    }

    inline bool BitmapPathTrieView::empty() const
    {
        return m_storage.nodes.empty();
    }

    inline size_t BitmapPathTrieView::nodeCount() const
    {
        return m_storage.nodes.size();
    }

    inline size_t BitmapPathTrieView::memoryUsage() const
    {
        return m_storage.nodes.size_bytes()
            + m_storage.poolSymbols.size_bytes()
            + m_storage.poolCase.size_bytes()
            + m_storage.bucketKeys.size_bytes()
            + m_storage.caseExceptions.size_bytes()
            + m_storage.casePositions.size_bytes();
    }

    inline const BitmapPathTrieStorage& BitmapPathTrieView::storage() const
    {
        return m_storage;
    }

    inline uint32 BitmapPathTrieView::find(oc::string_view path, bool caseSensitive, oc::span<char> outCanonical) const
    {
        assert((!outCanonical.data() || outCanonical.size() == path.size()) && "outCanonical must be the same size as path");

        if (!caseSensitive && !outCanonical.data())
            return findKey<false>(path, nullptr);

        char canonical[pathTrieDetail::MaxPath]; // written by findKey before it is read
        char* out = outCanonical.data() ? outCanonical.data() : canonical;

        const uint32 key = findKey<true>(path, out);
        if (key == InvalidKey)
            return InvalidKey;

        applyCaseExceptions(key, out);

        if (caseSensitive && memcmp(out, path.data(), path.size()) != 0)
            return InvalidKey;

        return key;
    }

    inline bool BitmapPathTrieView::contains(oc::string_view path, bool caseSensitive) const
    {
        return find(path, caseSensitive) != InvalidKey;
    }

    template<typename Func>
    inline void BitmapPathTrieView::forEach(Func func) const
    {
        forEachChild({}, true, func);
    }

    template<typename Func>
    inline void BitmapPathTrieView::forEachChild(oc::string_view prefix, bool recursive, Func func) const
    {
        if (m_storage.nodes.empty())
            return;

        assert(prefix.size() < pathTrieDetail::MaxPath && "Prefix too long");

        IterationContext context;
        context.prefix = prefix;
        context.recursive = recursive;
        visit(0, 0, context, func);
    }

    inline BitmapPathTrieStats BitmapPathTrieView::stats() const
    {
        BitmapPathTrieStats result;
        if (!m_storage.nodes.empty())
            collectStats(0, 1, false, result);

        result.caseExceptionKeys = m_storage.caseExceptions.empty() ? 0 : m_storage.caseExceptions.size() - 1; // excludes the sentinel
        result.casePositions = m_storage.casePositions.size();
        return result;
    }

    // Case insensitive lookup. With Output, out receives the path in the case stored in the trie (without case exceptions).
    template<bool Output>
    inline uint32 BitmapPathTrieView::findKey(oc::string_view path, char* out) const
    {
        using namespace pathTrieDetail;

        if (m_storage.nodes.empty() || path.size() >= MaxPath)
            return InvalidKey;

        Query query;
        if (!query.encode(path))
            return InvalidKey;

        const Node* nodes = m_storage.nodes.data();
        const Node* node = nodes;
        const uint32 length = static_cast<uint32>(path.size());
        uint32 pos = 0;

        for (;;)
        {
            // the whole label at once; the zero padding past the end of the query never matches a label symbol
            const uint32 label = node->label;
            const uint32 labelCount = packedCount(label);
            const uint64 queryWord = loadBits(query.symbols, size_t(pos) * SymbolBits);
            if (((queryWord ^ label) & packedMask(labelCount)) != 0)
                return InvalidKey;

            if constexpr (Output)
                writeSymbols(out + pos, label, labelCount, node->labelUpper);
            pos += labelCount;

            if (node->isLeaf())
                return matchLeaf<Output>(*node, query, pos, length, out) ? node->childOrKey : InvalidKey;

            if (node->isBucket())
                return findInBucket<Output>(*node, query, pos, out);

            // branch on the next query symbol, which is the end symbol once the path is consumed
            const uint32 symbol = static_cast<uint32>(queryWord >> (labelCount * SymbolBits)) & SymbolMask;
            if (!node->hasChild(symbol))
                return InvalidKey;

            const uint32 childIndex = node->getChildIndex(symbol);
            assert(childIndex < m_storage.nodes.size());
            node = &nodes[childIndex];

            // the end symbol child is the leaf of a key ending here; it consumes no character
            if (symbol != 0)
            {
                if constexpr (Output)
                    out[pos] = symbolToChar(symbol, node->branchUpper);
                ++pos;
            }
        }
    }

    template<bool Output>
    inline bool BitmapPathTrieView::matchLeaf(const Node& node, const pathTrieDetail::Query& query, uint32 pos, uint32 length, char* out) const
    {
        using namespace pathTrieDetail;

        // inline leaf: the rest of the path is in the node and must end exactly where the query ends
        if (!node.hasTail())
        {
            const uint64 symbols = node.inlineLeaf.symbols;
            const uint32 count = packedCount(symbols);
            if (pos + count != length)
                return false;

            const uint64 queryWord = loadBits(query.symbols, size_t(pos) * SymbolBits);
            if (((queryWord ^ symbols) & packedMask(count)) != 0)
                return false;

            if constexpr (Output)
                writeSymbols(out + pos, symbols, count, node.inlineLeaf.upper);
            return true;
        }

        // tail leaf: the rest of the path is in the symbol pool, compared 9 symbols at a time
        const uint32 tailOffset = static_cast<uint32>(node.tailLeaf.poolOffset);
        const uint32 tailLength = static_cast<uint32>(node.tailLeaf.length);
        if (pos + tailLength != length)
            return false;

        for (uint32 i = 0; i < tailLength; i += SymbolsPerLoad)
        {
            const uint32 count = oc::min(tailLength - i, SymbolsPerLoad);
            const uint64 tailWord = loadBits(m_storage.poolSymbols.data(), size_t(tailOffset + i) * SymbolBits);
            const uint64 queryWord = loadBits(query.symbols, size_t(pos + i) * SymbolBits);
            if (((tailWord ^ queryWord) & packedMask(count)) != 0)
                return false;

            if constexpr (Output)
            {
                const uint64 upper = m_storage.poolCase.empty() ? 0 : loadBits(m_storage.poolCase.data(), tailOffset + i);
                writeSymbols(out + pos + i, tailWord, count, upper);
            }
        }
        return true;
    }

    // Scans the front coded entries of a bucket. `matched` counts the query symbols (after pos) the previous entry
    // matched, which decides whether each entry can match, must be skipped, or ends the search (entries are sorted).
    template<bool Output>
    inline uint32 BitmapPathTrieView::findInBucket(const Node& node, const pathTrieDetail::Query& query, uint32 pos, char* out) const
    {
        using namespace pathTrieDetail;

        [[maybe_unused]] char entry[Output ? MaxPath : 1]; // Output: the current entry's characters, written before they are read
        uint32 symbolIndex = static_cast<uint32>(node.bucket.poolOffset);
        const uint32 keyCount = static_cast<uint32>(node.bucket.keyCount);
        uint32 matched = 0;
        uint32 previousLength = 0;

        for (uint32 i = 0; i < keyCount; ++i)
        {
            uint32 shared = 0;
            uint32 added = 0;
            readBucketEntryHeader(symbolIndex, previousLength, shared, added);
            if (shared < matched)
                return InvalidKey; // differs from the previous entry where that one still matched, so it sorts after the query

            // an entry sharing more than `matched` symbols with the previous one fails at the same position
            bool comparing = shared == matched;
            for (uint32 k = 0; comparing && k < added; k += SymbolsPerLoad)
            {
                // 9 symbols at a time; on a difference, the first differing symbol decides the order
                const uint32 count = oc::min(added - k, SymbolsPerLoad);
                const uint64 entryWord = loadBits(m_storage.poolSymbols.data(), size_t(symbolIndex + k) * SymbolBits);
                const uint64 queryWord = loadBits(query.symbols, size_t(pos + shared + k) * SymbolBits);
                const uint64 difference = (entryWord ^ queryWord) & packedMask(count);
                if (difference == 0)
                    continue;

                const uint32 first = oc::tzcnt(difference) / SymbolBits;
                const uint32 shift = first * SymbolBits;
                if (((entryWord >> shift) & SymbolMask) > ((queryWord >> shift) & SymbolMask))
                    return InvalidKey; // this and every following entry sort after the query

                comparing = false;
                matched = shared + k + first;
            }

            // decode the new characters even for skipped entries: later entries share them
            if constexpr (Output)
            {
                assert(pos + shared + added <= MaxPath);
                for (uint32 k = 0; k < added; ++k)
                    entry[shared + k] = symbolToChar(poolSymbol(symbolIndex + k), isPoolUpper(symbolIndex + k));
            }
            symbolIndex += added;
            previousLength = shared + added;

            if (comparing)
            {
                // the whole entry matched: it is the key if the query ends here too, otherwise a prefix of the query
                if (query.symbolAt(pos + previousLength) == 0)
                {
                    if constexpr (Output)
                        memcpy(out + pos, entry, previousLength);
                    return bucketKey(node.childOrKey + i);
                }
                matched = previousLength;
            }
        }
        return InvalidKey;
    }

    inline void BitmapPathTrieView::applyCaseExceptions(uint32 key, char* path) const
    {
        const oc::span<const BitmapPathTrieCaseException>& exceptions = m_storage.caseExceptions;
        if (exceptions.size() < 2)
            return; // only the sentinel, or nothing

        const BitmapPathTrieCaseException* begin = exceptions.data();
        const BitmapPathTrieCaseException* end = begin + exceptions.size() - 1; // excludes the sentinel
        const BitmapPathTrieCaseException* found = oc::lower_bound(begin, end, key,
            [](const BitmapPathTrieCaseException& entry, uint32 value) { return entry.key < value; });

        if (found == end || found->key != key)
            return;

        for (uint32 i = found->firstPosition; i < (found + 1)->firstPosition; ++i)
            path[m_storage.casePositions[i]] ^= 0x20; // only letters are stored, so this toggles their case
    }

    inline uint32 BitmapPathTrieView::poolSymbol(uint32 index) const
    {
        return static_cast<uint32>(pathTrieDetail::loadBits(m_storage.poolSymbols.data(), size_t(index) * pathTrieDetail::SymbolBits)) & pathTrieDetail::SymbolMask;
    }

    inline bool BitmapPathTrieView::isPoolUpper(uint32 index) const
    {
        // no case stream = no path has uppercase characters
        return !m_storage.poolCase.empty() && ((m_storage.poolCase[index >> 3] >> (index & 7)) & 1) != 0;
    }

    // Reads an explicit count (one symbol, or the escape and two symbols) and advances index past it.
    inline uint32 BitmapPathTrieView::readBucketCount(uint32& index) const
    {
        const uint32 value = poolSymbol(index++);
        if (value != pathTrieDetail::CountEscape)
            return value;

        const uint32 high = poolSymbol(index++);
        const uint32 low = poolSymbol(index++);
        return (high << pathTrieDetail::SymbolBits) | low;
    }

    // Reads a bucket entry's header and advances index to its new symbols. outShared = the symbols the entry shares with
    // the previous one (of length previousLength), outAdded = the new symbols that follow.
    inline void BitmapPathTrieView::readBucketEntryHeader(uint32& index, uint32 previousLength, uint32& outShared, uint32& outAdded) const
    {
        using namespace pathTrieDetail;

        const uint32 dropBits = m_storage.bucketDropBits;
        const uint32 dropEscape = (1u << dropBits) - 1;
        const uint32 addEscape = (1u << (SymbolBits - dropBits)) - 1;
        assert(dropBits >= MinDropBits && dropBits <= MaxDropBits);

        const uint32 header = poolSymbol(index++);
        uint32 drop = header & dropEscape;
        uint32 added = header >> dropBits;
        if (drop == dropEscape)
            drop = readBucketCount(index);
        if (added == addEscape)
            added = readBucketCount(index);

        assert(drop <= previousLength);
        outShared = previousLength - drop;
        outAdded = added;
    }

    inline uint32 BitmapPathTrieView::bucketKey(uint32 index) const
    {
        const uint32 bits = m_storage.bucketKeyBits;
        assert(bits > 0 && bits <= 32);
        return static_cast<uint32>(pathTrieDetail::loadBits(m_storage.bucketKeys.data(), size_t(index) * bits) & ((1ull << bits) - 1));
    }

    // Appends symbol's character to the path buffer. False when the walk must not go below this point: the path no longer
    // matches the prefix, or a sub directory was reported.
    template<typename Func>
    inline bool BitmapPathTrieView::appendSymbol(uint32 symbol, bool upper, uint32& length, IterationContext& context, Func& func) const
    {
        const char c = pathTrieDetail::symbolToChar(symbol, upper);

        if (length < context.prefix.size())
        {
            if (pathTrieDetail::charToSymbol(context.prefix[length]) != symbol)
                return false;
        }
        else if (!context.recursive && c == '/')
        {
            // every key in this sub directory shares this node, so the directory is reported exactly once
            func(BitmapPathTrieEntry{ oc::string_view(context.path, length), InvalidKey, true });
            return false;
        }

        assert(length < pathTrieDetail::MaxPath && "Path too long, raise MaxPath");
        if (length >= pathTrieDetail::MaxPath)
            return false;

        context.path[length++] = c;
        return true;
    }

    // Reports the key whose path is context.path[0, length), with its case exceptions applied during the callback.
    template<typename Func>
    inline void BitmapPathTrieView::reportKey(uint32 key, uint32 length, IterationContext& context, Func& func) const
    {
        applyCaseExceptions(key, context.path);
        func(BitmapPathTrieEntry{ oc::string_view(context.path, length), key, false });
        applyCaseExceptions(key, context.path); // toggles the case back
    }

    template<typename Func>
    inline void BitmapPathTrieView::visit(uint32 nodeIndex, uint32 length, IterationContext& context, Func& func) const
    {
        using namespace pathTrieDetail;

        assert(nodeIndex < m_storage.nodes.size());
        const Node* nodes = m_storage.nodes.data();
        const Node& node = nodes[nodeIndex];

        const uint32 label = node.label;
        const uint32 labelUpper = node.labelUpper;
        const uint32 labelCount = packedCount(label);

        // append the label; stops on a prefix mismatch or when a sub directory was reported
        for (uint32 i = 0; i < labelCount; ++i)
        {
            const uint32 symbol = (label >> (i * SymbolBits)) & SymbolMask;
            if (!appendSymbol(symbol, ((labelUpper >> i) & 1) != 0, length, context, func))
                return;
        }

        if (node.isBucket())
        {
            visitBucket(node, length, context, func);
            return;
        }

        // append the rest of a leaf's path (from the symbol pool or inline) and report its key
        if (node.isLeaf())
        {
            if (node.hasTail())
            {
                const uint32 tailOffset = static_cast<uint32>(node.tailLeaf.poolOffset);
                const uint32 tailLength = static_cast<uint32>(node.tailLeaf.length);
                for (uint32 i = 0; i < tailLength; ++i)
                {
                    if (!appendSymbol(poolSymbol(tailOffset + i), isPoolUpper(tailOffset + i), length, context, func))
                        return;
                }
            }
            else
            {
                const uint64 symbols = node.inlineLeaf.symbols;
                const uint64 upper = node.inlineLeaf.upper;
                const uint32 count = packedCount(symbols);
                for (uint32 i = 0; i < count; ++i)
                {
                    const uint32 symbol = static_cast<uint32>(symbols >> (i * SymbolBits)) & SymbolMask;
                    if (!appendSymbol(symbol, ((upper >> i) & 1) != 0, length, context, func))
                        return;
                }
            }

            if (length >= context.prefix.size())
                reportKey(node.childOrKey, length, context, func);
            return;
        }

        if (length < context.prefix.size())
        {
            // still inside the prefix: only one child can match
            const uint32 symbol = charToSymbol(context.prefix[length]);
            if (symbol >= AlphabetSize || !node.hasChild(symbol))
                return;

            const uint32 childIndex = node.getChildIndex(symbol);
            if (appendSymbol(symbol, nodes[childIndex].branchUpper, length, context, func))
                visit(childIndex, length, context, func);
            return;
        }

        // past the prefix: every child in symbol order, each one from the same path length
        uint32 childIndex = node.childOrKey;
        for (uint64 mask = node.childMask; mask != 0; mask &= mask - 1, ++childIndex)
        {
            const uint32 symbol = oc::tzcnt(mask);
            uint32 childLength = length; // the end symbol child (a key ending here) adds no character
            if (symbol != 0 && !appendSymbol(symbol, nodes[childIndex].branchUpper, childLength, context, func))
                continue;

            visit(childIndex, childLength, context, func);
        }
    }

    // Decodes a bucket's entries into context.path after length and reports the ones matching the prefix. Entries share
    // their first characters with the previous entry, which are still in the path. Non recursive, consecutive entries
    // in the same sub directory report that directory only once.
    template<typename Func>
    inline void BitmapPathTrieView::visitBucket(const Node& node, uint32 length, IterationContext& context, Func& func) const
    {
        using namespace pathTrieDetail;

        const uint32 prefixLength = static_cast<uint32>(context.prefix.size());
        const uint32 keyCount = static_cast<uint32>(node.bucket.keyCount);
        uint32 symbolIndex = static_cast<uint32>(node.bucket.poolOffset);
        uint32 previousLength = 0; // symbols of the previous entry after length
        bool hasDirectory = false; // a directory was reported for an earlier entry of this bucket
        uint32 directoryEnd = 0;   // length of that directory: the position of its '/'
        uint32 sharedWithDir = 0;  // characters after length the current entry shares with that entry

        for (uint32 i = 0; i < keyCount; ++i)
        {
            uint32 shared = 0;
            uint32 added = 0;
            readBucketEntryHeader(symbolIndex, previousLength, shared, added);
            sharedWithDir = oc::min(sharedWithDir, shared); // the minimum over every entry since that one
            previousLength = shared + added;

            // the first `shared` characters are still in the path from the previous entry; append the new ones
            uint32 entryLength = length + shared;
            assert(entryLength + added <= MaxPath);
            for (uint32 k = 0; k < added; ++k, ++symbolIndex)
                context.path[entryLength++] = symbolToChar(poolSymbol(symbolIndex), isPoolUpper(symbolIndex));

            // the characters before length were already checked against the prefix by the trie walk
            if (entryLength < prefixLength)
                continue;

            bool matchesPrefix = true;
            for (uint32 pos = length; pos < prefixLength && matchesPrefix; ++pos)
                matchesPrefix = charToSymbol(context.path[pos]) == charToSymbol(context.prefix[pos]);
            if (!matchesPrefix)
                continue;

            // non recursive: an entry in a sub directory reports that directory instead of itself
            if (!context.recursive)
            {
                const char* begin = context.path + oc::max(length, prefixLength);
                const char* end = context.path + entryLength;
                const char* slash = oc::find(begin, end, '/');
                if (slash != end)
                {
                    const uint32 directoryLength = static_cast<uint32>(slash - context.path);
                    if (hasDirectory && directoryEnd == directoryLength && length + sharedWithDir > directoryLength)
                        continue; // the directory an earlier entry already reported

                    func(BitmapPathTrieEntry{ oc::string_view(context.path, directoryLength), InvalidKey, true });
                    hasDirectory = true;
                    directoryEnd = directoryLength;
                    sharedWithDir = entryLength - length;
                    continue;
                }
            }

            reportKey(bucketKey(node.childOrKey + i), entryLength, context, func);
        }
    }

    // depth = the nodes a lookup visits up to and including this one.
    inline void BitmapPathTrieView::collectStats(uint32 nodeIndex, size_t depth, bool isEndChild, BitmapPathTrieStats& out) const
    {
        using namespace pathTrieDetail;

        assert(nodeIndex < m_storage.nodes.size());
        const Node& node = m_storage.nodes[nodeIndex];
        const uint32 labelCount = packedCount(node.label);

        if (node.isBucket())
        {
            const uint32 keyCount = static_cast<uint32>(node.bucket.keyCount);
            uint32 symbolIndex = static_cast<uint32>(node.bucket.poolOffset);
            uint32 previousLength = 0;
            for (uint32 i = 0; i < keyCount; ++i)
            {
                uint32 shared = 0;
                uint32 added = 0;
                const uint32 headerStart = symbolIndex;
                readBucketEntryHeader(symbolIndex, previousLength, shared, added);
                out.bucketHeaderSymbols += symbolIndex - headerStart;
                symbolIndex += added;
                previousLength = shared + added;
            }

            ++out.bucketNodes;
            out.bucketKeys += keyCount;
            out.bucketSymbols += symbolIndex - static_cast<uint32>(node.bucket.poolOffset);
            out.keyDepthSum += depth * keyCount;
            out.maxKeyDepth = oc::max(out.maxKeyDepth, depth);
            return;
        }

        if (node.isLeaf())
        {
            ++out.leafNodes;
            out.endLeafNodes += isEndChild ? 1 : 0;
            out.keyDepthSum += depth;
            out.maxKeyDepth = oc::max(out.maxKeyDepth, depth);

            if (node.hasTail())
            {
                ++out.tailLeafNodes;
                out.tailSymbols += static_cast<uint32>(node.tailLeaf.length);
            }
            else
            {
                out.inlineLeafSymbols += labelCount + packedCount(node.inlineLeaf.symbols);
            }
            return;
        }

        const uint32 childCount = oc::popcnt(node.childMask);
        if (childCount == 1)
        {
            ++out.chainNodes;
        }
        else
        {
            ++out.branchNodes;
            ++out.branchChildCounts[oc::bitWidth(childCount - 1)];
        }

        uint32 childIndex = node.childOrKey;
        for (uint64 mask = node.childMask; mask != 0; mask &= mask - 1, ++childIndex)
            collectStats(childIndex, depth + 1, oc::tzcnt(mask) == 0, out);
    }

    // ---- Implementation: BitmapPathTrie ----------------------------------------------------------------------------

    inline bool BitmapPathTrie::insert(oc::string_view path, uint32 key)
    {
        if (key > MaxKey || path.size() >= pathTrieDetail::MaxPath)
            return false;

        const size_t offset = m_pendingSymbols.size();
        assert(offset + path.size() < InvalidKey && "Too many path symbols for 32 bit offsets");

        for (size_t i = 0; i < path.size(); ++i)
        {
            const uint32 symbol = pathTrieDetail::charToSymbol(path[i]);
            if (symbol == pathTrieDetail::InvalidSymbol)
            {
                m_pendingSymbols.resize(offset);
                return false;
            }
            m_pendingSymbols.push_back(static_cast<uint8>(symbol | (pathTrieDetail::isUpperChar(path[i]) ? PendingUpperFlag : 0)));
        }

        PendingEntry entry;
        entry.symbolOffset = static_cast<uint32>(offset);
        entry.symbolCount = static_cast<uint32>(path.size());
        entry.key = key;
        m_pending.push_back(entry);
        return true;
    }

    inline bool BitmapPathTrie::build(uint32 maxBucketKeys)
    {
        assert(maxBucketKeys <= Node::Bucket::MaxKeys && "maxBucketKeys is larger than Node::Bucket::MaxKeys");

        m_nodes.clear();
        m_poolSymbols.clear();
        m_poolCase.clear();
        m_bucketKeys.clear();
        m_pendingBuckets.clear();
        m_pendingBucketKeys.clear();
        m_caseExceptions.clear();
        m_casePositions.clear();
        m_poolSymbolCount = 0;
        m_bucketKeyBits = 0;
        m_bucketDropBits = 0;
        m_maxBucketKeys = oc::min(maxBucketKeys, Node::Bucket::MaxKeys);
        // decides whether a case stream and case exceptions are needed at all
        m_hasUpper = oc::any_of(m_pendingSymbols.begin(), m_pendingSymbols.end(), [](uint8 symbol) { return (symbol & PendingUpperFlag) != 0; });

        if (m_pending.empty())
            return true;

        // Sorted by folded symbols: a path sorts before the paths it is a prefix of, which matches the end symbol being
        // bit 0. Stable, so the first inserted path decides the case of shared nodes.
        const uint8* symbols = m_pendingSymbols.data();
        auto pathLess = [symbols](const PendingEntry& a, const PendingEntry& b)
        {
            return oc::lexicographical_compare(
                symbols + a.symbolOffset, symbols + a.symbolOffset + a.symbolCount,
                symbols + b.symbolOffset, symbols + b.symbolOffset + b.symbolCount,
                [](uint8 x, uint8 y) { return (x & pathTrieDetail::SymbolMask) < (y & pathTrieDetail::SymbolMask); });
        };
        oc::stable_sort(m_pending.begin(), m_pending.end(), pathLess);

        for (size_t i = 1; i < m_pending.size(); ++i)
            if (!pathLess(m_pending[i - 1], m_pending[i]))
                return false; // duplicate path

        const uint32 rootIndex = allocateNodes(1);
        buildNode(rootIndex, 0, static_cast<uint32>(m_pending.size()), 0); // nodes, leaf tails and bucket nodes
        writeBuckets();                                                    // bucket entries: needs every bucket to pick the header split
        packBucketKeys();                                                  // needs every bucket key to pick the key width
        buildCaseExceptions();                                             // needs the finished trie to see which case it returns
        return true;
    }

    inline void BitmapPathTrie::clear()
    {
        clearPending();
        m_nodes.clear();
        m_poolSymbols.clear();
        m_poolCase.clear();
        m_bucketKeys.clear();
        m_caseExceptions.clear();
        m_casePositions.clear();
        m_poolSymbolCount = 0;
        m_bucketKeyBits = 0;
        m_bucketDropBits = 0;
    }

    inline void BitmapPathTrie::clearPending()
    {
        m_pendingSymbols.clear();
        m_pendingSymbols.shrink_to_fit();
        m_pending.clear();
        m_pending.shrink_to_fit();
    }

    inline BitmapPathTrieStorage BitmapPathTrie::storage() const
    {
        BitmapPathTrieStorage result;
        result.nodes = { m_nodes.data(), m_nodes.size() };
        result.poolSymbols = { m_poolSymbols.data(), m_poolSymbols.size() };
        result.poolCase = { m_poolCase.data(), m_poolCase.size() };
        result.bucketKeys = { m_bucketKeys.data(), m_bucketKeys.size() };
        result.caseExceptions = { m_caseExceptions.data(), m_caseExceptions.size() };
        result.casePositions = { m_casePositions.data(), m_casePositions.size() };
        result.bucketKeyBits = m_bucketKeyBits;
        result.bucketDropBits = m_bucketDropBits;
        return result;
    }

    inline BitmapPathTrieView BitmapPathTrie::view() const
    {
        return BitmapPathTrieView(storage());
    }

    inline uint32 BitmapPathTrie::symbolAt(const PendingEntry& entry, uint32 pos) const
    {
        return pos < entry.symbolCount ? (m_pendingSymbols[entry.symbolOffset + pos] & pathTrieDetail::SymbolMask) : 0;
    }

    inline bool BitmapPathTrie::isUpper(const PendingEntry& entry, uint32 pos) const
    {
        return pos < entry.symbolCount && (m_pendingSymbols[entry.symbolOffset + pos] & PendingUpperFlag) != 0;
    }

    // Stores count symbols of entry from pos on as node's label (symbols and uppercase mask).
    inline void BitmapPathTrie::packLabel(const PendingEntry& entry, uint32 pos, uint32 count, Node& node) const
    {
        assert(count <= Node::LabelSymbols);
        uint32 label = 0;
        uint32 labelUpper = 0;
        for (uint32 i = 0; i < count; ++i)
        {
            label |= symbolAt(entry, pos + i) << (i * pathTrieDetail::SymbolBits);
            labelUpper |= (isUpper(entry, pos + i) ? 1u : 0u) << i;
        }
        node.label = label;
        node.labelUpper = labelUpper;
    }

    inline uint32 BitmapPathTrie::allocateNodes(uint32 count)
    {
        const size_t first = m_nodes.size();
        assert(first + count <= pathTrieDetail::MaxChildOrKey && "Too many nodes for 26 bit child indices");
        m_nodes.resize(first + count);
        return static_cast<uint32>(first);
    }

    // The symbols after depth shared by the whole sorted range [begin, end), which holds at least 2 entries. As the range
    // is sorted, that is the prefix its first and last entry share. Paths are unique, so those two always differ before
    // both reach their end.
    inline uint32 BitmapPathTrie::sharedSymbolCount(uint32 begin, uint32 end, uint32 depth) const
    {
        assert(end - begin >= 2);
        const PendingEntry& first = m_pending[begin];
        const PendingEntry& last = m_pending[end - 1];

        uint32 shared = 0;
        while (symbolAt(first, depth + shared) == symbolAt(last, depth + shared))
            ++shared;
        return shared;
    }

    // Builds the node for the sorted range [begin, end), which all share their first depth symbols. Shared nodes take
    // their case from the first entry of the range.
    inline void BitmapPathTrie::buildNode(uint32 nodeIndex, uint32 begin, uint32 end, uint32 depth)
    {
        // a single key becomes a leaf, a small range a bucket, everything else a node with a label and children
        if (end - begin == 1)
        {
            buildLeaf(nodeIndex, m_pending[begin], depth);
            return;
        }

        if (end - begin <= m_maxBucketKeys)
        {
            buildBucket(nodeIndex, begin, end, depth);
            return;
        }

        const PendingEntry& first = m_pending[begin];
        const uint32 shared = sharedSymbolCount(begin, end, depth);
        const uint32 labelCount = oc::min(shared, Node::LabelSymbols);
        const uint32 branchDepth = depth + labelCount;

        if (shared > labelCount)
        {
            // the shared run is longer than the label: continue in a single child
            const uint32 symbol = symbolAt(first, branchDepth);
            const uint32 childIndex = allocateNodes(1);
            {
                Node& node = m_nodes[nodeIndex];
                packLabel(first, depth, labelCount, node);
                node.childMask = 1ull << symbol;
                node.childOrKey = childIndex;
            }

            buildNode(childIndex, begin, end, branchDepth + 1);
            m_nodes[childIndex].branchUpper = isUpper(first, branchDepth) ? 1 : 0; // the child stores the branch symbol's case
            return;
        }

        // the range splits at branchDepth; entries with the same branch symbol are contiguous and in symbol order
        uint64 mask = 0;
        uint32 childCount = 0;
        for (uint32 i = begin; i < end; ++i)
        {
            const uint64 bit = 1ull << symbolAt(m_pending[i], branchDepth);
            if ((mask & bit) == 0)
            {
                mask |= bit;
                ++childCount;
            }
        }

        // all children in one block, so they are contiguous, which Node::getChildIndex() relies on
        const uint32 firstChild = allocateNodes(childCount);
        {
            Node& node = m_nodes[nodeIndex];
            packLabel(first, depth, labelCount, node);
            node.childMask = mask;
            node.childOrKey = firstChild;
        }

        // one child per group of entries with the same branch symbol
        uint32 childIndex = firstChild;
        uint32 groupBegin = begin;
        while (groupBegin < end)
        {
            const PendingEntry& groupFirst = m_pending[groupBegin];
            const uint32 symbol = symbolAt(groupFirst, branchDepth);
            uint32 groupEnd = groupBegin + 1;
            while (groupEnd < end && symbolAt(m_pending[groupEnd], branchDepth) == symbol)
                ++groupEnd;

            if (symbol == 0)
            {
                // a key ends here; paths are unique, so this group holds exactly one entry
                assert(groupEnd - groupBegin == 1);
                buildLeaf(childIndex, groupFirst, branchDepth);
            }
            else
            {
                buildNode(childIndex, groupBegin, groupEnd, branchDepth + 1);
                m_nodes[childIndex].branchUpper = isUpper(groupFirst, branchDepth) ? 1 : 0;
            }

            ++childIndex;
            groupBegin = groupEnd;
        }
    }

    // Turns the sorted range [begin, end) into a bucket node. The prefix the whole range shares (up to 5 symbols) becomes
    // the node label. writeBuckets() writes the entries once every bucket is known; the keys are collected in bucket order
    // and bit packed by packBucketKeys().
    inline void BitmapPathTrie::buildBucket(uint32 nodeIndex, uint32 begin, uint32 end, uint32 depth)
    {
        const uint32 labelCount = oc::min(sharedSymbolCount(begin, end, depth), Node::LabelSymbols);
        const uint32 firstKey = static_cast<uint32>(m_pendingBucketKeys.size());
        assert(firstKey <= pathTrieDetail::MaxChildOrKey && "Too many bucket keys for 26 bit key indices");

        Node& node = m_nodes[nodeIndex];
        packLabel(m_pending[begin], depth, labelCount, node);
        node.childOrKey = firstKey;
        node.setKind(Node::ENodeKind::Bucket);
        node.bucket.keyCount = end - begin; // poolOffset is set by writeBuckets()

        for (uint32 i = begin; i < end; ++i)
            m_pendingBucketKeys.push_back(m_pending[i].key);

        m_pendingBuckets.push_back(PendingBucket{ nodeIndex, begin, end, depth + labelCount });
    }

    // Stores the rest of entry after depth: up to 5 symbols in the label, up to 9 more inline, anything longer in the
    // symbol pool.
    inline void BitmapPathTrie::buildLeaf(uint32 nodeIndex, const PendingEntry& entry, uint32 depth)
    {
        assert(depth <= entry.symbolCount);
        const uint32 remaining = entry.symbolCount - depth;
        const uint32 labelCount = oc::min(remaining, Node::LabelSymbols);
        const uint32 rest = remaining - labelCount;

        Node& node = m_nodes[nodeIndex];
        packLabel(entry, depth, labelCount, node);
        node.childOrKey = entry.key;
        node.childMask = 0;

        const uint32 restDepth = depth + labelCount;
        if (rest <= Node::InlineLeaf::MaxSymbols)
        {
            uint64 symbols = 0;
            uint64 upper = 0;
            for (uint32 i = 0; i < rest; ++i)
            {
                symbols |= uint64(symbolAt(entry, restDepth + i)) << (i * pathTrieDetail::SymbolBits);
                upper |= uint64(isUpper(entry, restDepth + i) ? 1 : 0) << i;
            }
            node.inlineLeaf.symbols = symbols;
            node.inlineLeaf.upper = upper;
            node.setKind(Node::ENodeKind::InlineLeaf);
            return;
        }

        assert(rest <= Node::TailLeaf::MaxLength);
        node.tailLeaf.poolOffset = m_poolSymbolCount;
        node.tailLeaf.length = rest;
        node.setKind(Node::ENodeKind::TailLeaf);
        for (uint32 i = 0; i < rest; ++i)
            appendPoolSymbol(symbolAt(entry, restDepth + i), isUpper(entry, restDepth + i));
    }

    // func(drop, added, entry, firstAddedPos) for every entry of a bucket: the symbols to drop from the end of the
    // previous entry, the number of new symbols, and the position of the first new symbol in entry.
    template<typename Func>
    inline void BitmapPathTrie::forEachBucketEntry(const PendingBucket& bucket, Func func) const
    {
        for (uint32 i = bucket.begin; i < bucket.end; ++i)
        {
            const PendingEntry& entry = m_pending[i];
            uint32 shared = 0;
            uint32 previousLength = 0;
            if (i > bucket.begin)
            {
                // symbols shared with the previous entry; the first entry shares nothing
                const PendingEntry& previous = m_pending[i - 1];
                const uint32 maxShared = oc::min(entry.symbolCount, previous.symbolCount) - bucket.entryDepth;
                while (shared < maxShared && symbolAt(entry, bucket.entryDepth + shared) == symbolAt(previous, bucket.entryDepth + shared))
                    ++shared;
                previousLength = previous.symbolCount - bucket.entryDepth;
            }

            func(previousLength - shared, entry.symbolCount - bucket.entryDepth - shared, entry, bucket.entryDepth + shared);
        }
    }

    // The symbols an explicit count takes, see appendPoolCount().
    inline uint32 BitmapPathTrie::countSymbols(uint32 count)
    {
        return count < pathTrieDetail::CountEscape ? 1 : 3;
    }

    inline void BitmapPathTrie::appendPoolSymbol(uint32 symbol, bool upper)
    {
        assert(m_poolSymbolCount < InvalidKey && "Too many pool symbols for 32 bit offsets");

        // both streams keep the padding past the last used byte, so readers can always do a 64 bit load;
        // a symbol spans up to 2 bytes
        const size_t bit = size_t(m_poolSymbolCount) * pathTrieDetail::SymbolBits;
        const size_t byte = bit >> 3;
        if (m_poolSymbols.size() < byte + 2 + pathTrieDetail::StreamPadding)
            m_poolSymbols.resize(byte + 2 + pathTrieDetail::StreamPadding);

        const uint32 word = symbol << (bit & 7);
        m_poolSymbols[byte] |= static_cast<uint8>(word);
        m_poolSymbols[byte + 1] |= static_cast<uint8>(word >> 8);

        if (m_hasUpper)
        {
            const size_t caseByte = m_poolSymbolCount >> 3;
            if (m_poolCase.size() < caseByte + 1 + pathTrieDetail::StreamPadding)
                m_poolCase.resize(caseByte + 1 + pathTrieDetail::StreamPadding);
            if (upper)
                m_poolCase[caseByte] |= static_cast<uint8>(1u << (m_poolSymbolCount & 7));
        }
        assert(m_hasUpper || !upper);

        ++m_poolSymbolCount;
    }

    inline void BitmapPathTrie::appendPoolCount(uint32 count)
    {
        using namespace pathTrieDetail;

        assert(count <= MaxCount);
        if (count < CountEscape)
        {
            appendPoolSymbol(count, false);
            return;
        }

        appendPoolSymbol(CountEscape, false);
        appendPoolSymbol(count >> SymbolBits, false);
        appendPoolSymbol(count & SymbolMask, false);
    }

    // Picks the header split between drop and added count that needs the fewest pool symbols over every bucket, then
    // writes every bucket's entries and points the bucket nodes at them.
    inline void BitmapPathTrie::writeBuckets()
    {
        using namespace pathTrieDetail;

        if (m_pendingBuckets.empty())
            return;

        // the header symbols (escaped counts included) each possible split would need
        size_t headerSymbols[MaxDropBits + 1] = {};
        for (const PendingBucket& bucket : m_pendingBuckets)
        {
            forEachBucketEntry(bucket, [&headerSymbols](uint32 drop, uint32 added, const PendingEntry&, uint32)
            {
                for (uint32 dropBits = MinDropBits; dropBits <= MaxDropBits; ++dropBits)
                {
                    const uint32 dropEscape = (1u << dropBits) - 1;
                    const uint32 addEscape = (1u << (SymbolBits - dropBits)) - 1;
                    headerSymbols[dropBits] += 1
                        + (drop >= dropEscape ? countSymbols(drop) : 0)
                        + (added >= addEscape ? countSymbols(added) : 0);
                }
            });
        }

        m_bucketDropBits = MinDropBits;
        for (uint32 dropBits = MinDropBits; dropBits <= MaxDropBits; ++dropBits)
            if (headerSymbols[dropBits] < headerSymbols[m_bucketDropBits])
                m_bucketDropBits = dropBits;

        // every entry as a header symbol, the escaped counts (if any) and the new symbols
        const uint32 dropEscape = (1u << m_bucketDropBits) - 1;
        const uint32 addEscape = (1u << (SymbolBits - m_bucketDropBits)) - 1;
        for (const PendingBucket& bucket : m_pendingBuckets)
        {
            m_nodes[bucket.nodeIndex].bucket.poolOffset = m_poolSymbolCount;

            forEachBucketEntry(bucket, [&](uint32 drop, uint32 added, const PendingEntry& entry, uint32 firstAddedPos)
            {
                const uint32 dropField = oc::min(drop, dropEscape);
                const uint32 addField = oc::min(added, addEscape);
                appendPoolSymbol(dropField | (addField << m_bucketDropBits), false);
                if (dropField == dropEscape)
                    appendPoolCount(drop);
                if (addField == addEscape)
                    appendPoolCount(added);

                for (uint32 pos = firstAddedPos; pos < entry.symbolCount; ++pos)
                    appendPoolSymbol(symbolAt(entry, pos), isUpper(entry, pos));
            });
        }

        m_pendingBuckets.clear();
        m_pendingBuckets.shrink_to_fit();
    }

    // Packs the bucket keys at the smallest bit width that fits the largest one.
    inline void BitmapPathTrie::packBucketKeys()
    {
        if (m_pendingBucketKeys.empty())
            return;

        const uint32 maxKey = *oc::max_element(m_pendingBucketKeys.begin(), m_pendingBucketKeys.end());
        m_bucketKeyBits = oc::max(1u, oc::bitWidth(maxKey));

        // padded, so readers can always do a 64 bit load
        m_bucketKeys.resize((m_pendingBucketKeys.size() * m_bucketKeyBits + 7) / 8 + pathTrieDetail::StreamPadding);
        for (size_t i = 0; i < m_pendingBucketKeys.size(); ++i)
        {
            const size_t bit = i * m_bucketKeyBits;
            uint64 word = 0;
            memcpy(&word, &m_bucketKeys[bit >> 3], sizeof(word));
            word |= uint64(m_pendingBucketKeys[i]) << (bit & 7);
            memcpy(&m_bucketKeys[bit >> 3], &word, sizeof(word));
        }

        m_pendingBucketKeys.clear();
        m_pendingBucketKeys.shrink_to_fit();
    }

    // Compares the case every key gets from the trie with its original case and records the differences.
    inline void BitmapPathTrie::buildCaseExceptions()
    {
        if (!m_hasUpper)
            return; // everything is lowercase, so no key can differ from the trie

        struct KeyPositions
        {
            uint32 key = InvalidKey;
            uint32 firstPosition = 0;
            uint32 count = 0;
        };
        oc::vector<KeyPositions> keys;
        oc::vector<uint16> positions;

        const BitmapPathTrieView trieView = view(); // no case exceptions yet
        char lower[pathTrieDetail::MaxPath] = {};
        char canonical[pathTrieDetail::MaxPath] = {};

        for (const PendingEntry& entry : m_pending)
        {
            // look up the lowercase path for the case the trie returns, then diff that with the original case
            for (uint32 i = 0; i < entry.symbolCount; ++i)
                lower[i] = pathTrieDetail::symbolToChar(symbolAt(entry, i), false);

            const uint32 key = trieView.findKey<true>(oc::string_view(lower, entry.symbolCount), canonical);
            assert(key == entry.key);
            (void)key;

            const uint32 firstPosition = static_cast<uint32>(positions.size());
            for (uint32 i = 0; i < entry.symbolCount; ++i)
                if (pathTrieDetail::isUpperChar(canonical[i]) != isUpper(entry, i))
                    positions.push_back(static_cast<uint16>(i));

            if (positions.size() > firstPosition)
                keys.push_back(KeyPositions{ entry.key, firstPosition, static_cast<uint32>(positions.size()) - firstPosition });
        }

        if (keys.empty())
            return;

        // sorted by key for the binary search in applyCaseExceptions(); the positions follow in the same order
        oc::sort(keys.begin(), keys.end(), [](const KeyPositions& a, const KeyPositions& b) { return a.key < b.key; });

        m_caseExceptions.reserve(keys.size() + 1);
        m_casePositions.reserve(positions.size());
        for (const KeyPositions& keyPositions : keys)
        {
            m_caseExceptions.push_back(BitmapPathTrieCaseException{ keyPositions.key, static_cast<uint32>(m_casePositions.size()) });
            m_casePositions.insert(m_casePositions.end(), positions.begin() + keyPositions.firstPosition, positions.begin() + keyPositions.firstPosition + keyPositions.count);
        }
        // sentinel, so every real entry has a "next" entry
        m_caseExceptions.push_back(BitmapPathTrieCaseException{ InvalidKey, static_cast<uint32>(m_casePositions.size()) });
    }
}

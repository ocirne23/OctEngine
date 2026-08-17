/*module;

#pragma warning( push )
#pragma warning(disable: 5260) // warning C5260: the constant variable has internal linkage in an included header file context, but external linkage in imported header unit context
#include <EASTL/bonus/ring_buffer.h>
#pragma warning( pop )

export module ExpiryCache;

import Core;


export struct IntrusiveLinkedListNode
{
    float val;
    IntrusiveLinkedListNode* next = nullptr;
    IntrusiveLinkedListNode* prev = nullptr;
};

// concept to make sure value type extends IntrusiveLinkedListNode
//template <typename T>
//concept IntrusiveLinkedListNodeConcept = requires(T value) {
//    { value.next } -> std::same_as<IntrusiveLinkedListNode*>;
//    { value.prev } -> std::same_as<IntrusiveLinkedListNode*>;
//};
//requires IntrusiveLinkedListNodeConcept<Value>

export template <typename Value>
class ExpiryCache
{
public:

    using Entry = oc::pair<float, eastl::vector<Value*>>;
    eastl::ring_buffer<Entry> data;

    size_t insert(float expiry, Value* val)
    {
        // binary search for the right position to insert
        size_t left = 0, right = data.size();
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            if (data[mid].first < expiry) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        // insert the entry at the found position
        return 0;
    }
};

export float foo()
{
    ExpiryCache<IntrusiveLinkedListNode> cache;
    IntrusiveLinkedListNode awa;
    cache.insert(0.2f, &awa);
    return 0.0f;
}*/
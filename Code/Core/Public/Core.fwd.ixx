export module Core.fwd;

import <type_traits>;

export extern "C++"
{
	class Timer;
	class Time;
	class Window;
	struct Camera;
	struct Frustum;
	struct Transform;
	struct Rect;
	struct AABB;
	struct Sphere;
	class Allocator;
	template<bool ThreadSafe = false> class BitRangeAllocator;
	template<typename T> class LockedList;
	template <typename T> class LockFreeList;
	template<size_t Size> class RefCountTracker;

	class RefCheckable;
	template <typename T> concept IsRefCheckable = std::is_base_of<RefCheckable, T>::value;
	template<IsRefCheckable T> class CheckedPtr;
}
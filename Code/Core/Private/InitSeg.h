#pragma once

// THE single authority for static-init / teardown ordering of engine globals. Construction runs top
// to bottom, destruction (atexit, main thread) runs bottom to top. Section names sort
// lexicographically; a global WITHOUT a pragma lands in plain ".CRT$XCU", which constructs after the
// XCA sections but BEFORE every numbered XCU section — so plain globals destruct after all of them.
//
// Use at the global's definition (the pragma applies to the whole TU):
//     OC_INIT_SEG(OC_SEG_WORLD)
//     World world;
//
// #pragma directives don't macro-expand their arguments; the __pragma() operator does — hence the
// macro below. Available everywhere via forceinclude.h (/FI), no include needed.

// -- Core bootstrap: allocator first (global new/delete), then the profiler and the memory tracker
// that hook it. Constructed before everything, destructed after everything.
#define OC_SEG_CORE_ALLOCATOR      ".CRT$XCA"
#define OC_SEG_CORE_PROFILER       ".CRT$XCA1"
#define OC_SEG_CORE_MEMORY_TRACKER ".CRT$XCA2"

// -- ... plain ".CRT$XCU" globals construct here: input, audio, spatialIndex,
//       entityAllocator, scriptContext, scriptHost, assetRegistry, time, ... --

// -- RendererVK internals: instance -> device + GPU allocator -> renderer + XR session -> data
// managers. Teardown runs managers first, instance last.
#define OC_SEG_VK_INSTANCE ".CRT$XCU1"
#define OC_SEG_VK_DEVICE   ".CRT$XCU2" // device, gpuAllocator
#define OC_SEG_VK_RENDERER ".CRT$XCU3" // rendererVK, openXR
#define OC_SEG_VK_DATA     ".CRT$XCU4" // meshDataManager, meshStreamer, stagingManager, textureManager, textureStreamer

// -- Teardown-ordered engine globals, destructing BEFORE all of the above (reverse order: ui first).
// Every EntityPtr holder must sit above jobSystem here: destroying an entity reaches
// PerWorker::local(), which needs the main thread's live worker context (~JobSystem nulls it).
#define OC_SEG_JOB_SYSTEM      ".CRT$XCU5" // ~JobSystem joins workers, after every entity holder released
#define OC_SEG_PHYSICS         ".CRT$XCU51" // ~PhysicsWorld's b3DestroyWorld fans tasks onto the job system, so it must beat ~JobSystem; bodies died in ~World above
#define OC_SEG_NETWORK_MANAGER ".CRT$XCU6" // host closes after ~World's NetworkComponents unregistered
#define OC_SEG_SCRIPT_EVENTS   ".CRT$XCU7" // undrained EntityChange queue holds EntityPtrs
#define OC_SEG_WORLD           ".CRT$XCU8" // root entities die, then caches -> live renderer/audio
#define OC_SEG_UI              ".CRT$XCU9" // panel EntityPtrs + EntityChange queues

// -- Procedural world systems: FIRST to destruct — they free render chunks/sectors (-> renderer),
// static collider bodies (-> physics), and their dtors may wait on in-flight jobs (-> job system).
// Order among them is link-order-undefined (same section, different TUs) — their dtors are
// independent of each other; shared terrain data is handed out as shared_ptr copies.
#define OC_SEG_PROCEDURAL ".CRT$XCUA" // terrain, terrainCollider, ocean, scatter

// -- Nav: navSystem's dtor waits on its in-flight field-build jobs (-> job system); holds no
// entities and touches no other global, so it may go first of all.
#define OC_SEG_NAV ".CRT$XCUB" // navSystem

#define OC_INIT_SEG(seg) __pragma(warning(disable: 4075)) __pragma(init_seg(seg)) __pragma(warning(default: 4075))

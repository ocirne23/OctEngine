export module Core.VrSession;

import Core;

// Abstract handle-provider for an active OpenXR (VR) session, living in shared Core so the Input and
// RendererVK libs need not depend on each other. The renderer owns the real session (instance / session /
// reference-space + frame loop) and implements this interface; the Input lib consumes it to build its own
// controller action set; main.cpp bridges the two. Handles are opaque (void* / int64) so Core stays free
// of any OpenXR types - consumers cast them back to the Xr* handles they already include.
export class IVrSession
{
public:
    virtual ~IVrSession() = default;

    virtual void* xrInstance() const = 0;
    virtual void* xrSession() const = 0;
    virtual void* xrReferenceSpace() const = 0;     // LOCAL space; controller poses are located against it
    virtual int64 predictedDisplayTime() const = 0; // latest XR frame's predicted display time
};

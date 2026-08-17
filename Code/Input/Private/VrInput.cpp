module;

#include <cstdio>
#include <openxr/openxr.h>

module Input;

import Core;
import Core.glm;
import Core.VrSession;

import :VrInput;

namespace
{
    bool xrOk(XrResult r, const char* what)
    {
        if (XR_SUCCEEDED(r))
            return true;
        printf("VrInput: %s failed (%d)\n", what, (int)r);
        return false;
    }

    // OpenXR name fields are fixed char arrays; copy with truncation + null-termination.
    void copyName(char* dst, size_t cap, const char* src)
    {
        size_t i = 0;
        for (; src[i] && i + 1 < cap; ++i) dst[i] = src[i];
        dst[i] = '\0';
    }

    const char* const HAND_SIDE[2] = { "left", "right" };
}

bool VrInput::initialize(IVrSession* vrSession)
{
    if (!vrSession)
        return false; // VR disabled / no runtime
    m_vrSession = vrSession;
    m_instance = (XrInstance)vrSession->xrInstance();
    m_session = (XrSession)vrSession->xrSession();
    m_referenceSpace = (XrSpace)vrSession->xrReferenceSpace();
    if (!m_instance || !m_session || !m_referenceSpace)
        return false;

    XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    copyName(setInfo.actionSetName, sizeof(setInfo.actionSetName), "gameplay");
    copyName(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "Gameplay");
    setInfo.priority = 0;
    if (!xrOk(xrCreateActionSet(m_instance, &setInfo, &m_actionSet), "xrCreateActionSet"))
        return false;

    auto makeAction = [&](const char* name, const char* localized, XrActionType type, XrAction& outAction) -> bool
    {
        XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
        copyName(info.actionName, sizeof(info.actionName), name);
        copyName(info.localizedActionName, sizeof(info.localizedActionName), localized);
        info.actionType = type;
        return xrOk(xrCreateAction(m_actionSet, &info, &outAction), "xrCreateAction");
    };

    if (!makeAction("move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, m_moveAction)) return false;
    if (!makeAction("turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT, m_turnAction)) return false;

    // Per-hand pose / analog / button actions (distinct actions per hand rather than subaction paths).
    char nameBuf[XR_MAX_ACTION_NAME_SIZE];
    char locBuf[XR_MAX_LOCALIZED_ACTION_NAME_SIZE];
    auto makeHandAction = [&](const char* base, const char* localizedBase, uint32 hand, XrActionType type, XrAction& out) -> bool
    {
        snprintf(nameBuf, sizeof(nameBuf), "%s_%s", base, HAND_SIDE[hand]);
        snprintf(locBuf, sizeof(locBuf), "%s %s", localizedBase, HAND_SIDE[hand]);
        return makeAction(nameBuf, locBuf, type, out);
    };
    for (uint32 hand = 0; hand < HAND_COUNT; ++hand)
    {
        if (!makeHandAction("pose", "Pose", hand, XR_ACTION_TYPE_POSE_INPUT, m_poseAction[hand])) return false;
        if (!makeHandAction("trigger", "Trigger", hand, XR_ACTION_TYPE_FLOAT_INPUT, m_triggerAction[hand])) return false;
        if (!makeHandAction("grip", "Grip", hand, XR_ACTION_TYPE_FLOAT_INPUT, m_gripAction[hand])) return false;
        if (!makeHandAction("primary", "Primary", hand, XR_ACTION_TYPE_BOOLEAN_INPUT, m_primaryAction[hand])) return false;
        if (!makeHandAction("secondary", "Secondary", hand, XR_ACTION_TYPE_BOOLEAN_INPUT, m_secondaryAction[hand])) return false;
    }

    auto toPath = [&](const char* str) -> XrPath
    {
        XrPath p = XR_NULL_PATH;
        if (str && str[0])
            xrStringToPath(m_instance, str, &p);
        return p;
    };

    // Suggested bindings per controller profile. Each profile lists only the inputs it actually exposes
    // (empty = skipped) so one unsupported path doesn't fail the whole profile's suggestion. The runtime
    // activates whichever profile matches the connected device.
    struct Binding { XrAction action; const char* path; };
    auto suggest = [&](const char* profile, oc::initializer_list<Binding> list)
    {
        oc::vector<XrActionSuggestedBinding> binds;
        for (const Binding& b : list)
        {
            XrPath p = toPath(b.path);
            if (p != XR_NULL_PATH)
                binds.push_back({ b.action, p });
        }
        if (binds.empty())
            return;
        XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile = toPath(profile);
        suggested.countSuggestedBindings = (uint32)binds.size();
        suggested.suggestedBindings = binds.data();
        xrSuggestInteractionProfileBindings(m_instance, &suggested);
    };

    const uint32 L = (uint32)EVrHand::Left, R = (uint32)EVrHand::Right;

    // Oculus Touch (Quest/Rift): X/Y on left, A/B on right.
    suggest("/interaction_profiles/oculus/touch_controller", {
        { m_moveAction,           "/user/hand/left/input/thumbstick" },
        { m_turnAction,           "/user/hand/right/input/thumbstick" },
        { m_poseAction[L],        "/user/hand/left/input/grip/pose" },
        { m_poseAction[R],        "/user/hand/right/input/grip/pose" },
        { m_triggerAction[L],     "/user/hand/left/input/trigger/value" },
        { m_triggerAction[R],     "/user/hand/right/input/trigger/value" },
        { m_gripAction[L],        "/user/hand/left/input/squeeze/value" },
        { m_gripAction[R],        "/user/hand/right/input/squeeze/value" },
        { m_primaryAction[L],     "/user/hand/left/input/x/click" },
        { m_secondaryAction[L],   "/user/hand/left/input/y/click" },
        { m_primaryAction[R],     "/user/hand/right/input/a/click" },
        { m_secondaryAction[R],   "/user/hand/right/input/b/click" },
    });

    // Valve Index: A/B on both controllers.
    suggest("/interaction_profiles/valve/index_controller", {
        { m_moveAction,           "/user/hand/left/input/thumbstick" },
        { m_turnAction,           "/user/hand/right/input/thumbstick" },
        { m_poseAction[L],        "/user/hand/left/input/grip/pose" },
        { m_poseAction[R],        "/user/hand/right/input/grip/pose" },
        { m_triggerAction[L],     "/user/hand/left/input/trigger/value" },
        { m_triggerAction[R],     "/user/hand/right/input/trigger/value" },
        { m_gripAction[L],        "/user/hand/left/input/squeeze/value" },
        { m_gripAction[R],        "/user/hand/right/input/squeeze/value" },
        { m_primaryAction[L],     "/user/hand/left/input/a/click" },
        { m_secondaryAction[L],   "/user/hand/left/input/b/click" },
        { m_primaryAction[R],     "/user/hand/right/input/a/click" },
        { m_secondaryAction[R],   "/user/hand/right/input/b/click" },
    });

    // Windows Mixed Reality: squeeze is a click (bool->float conversion), no A/B/X/Y face buttons.
    suggest("/interaction_profiles/microsoft/motion_controller", {
        { m_moveAction,           "/user/hand/left/input/thumbstick" },
        { m_turnAction,           "/user/hand/right/input/thumbstick" },
        { m_poseAction[L],        "/user/hand/left/input/grip/pose" },
        { m_poseAction[R],        "/user/hand/right/input/grip/pose" },
        { m_triggerAction[L],     "/user/hand/left/input/trigger/value" },
        { m_triggerAction[R],     "/user/hand/right/input/trigger/value" },
        { m_gripAction[L],        "/user/hand/left/input/squeeze/click" },
        { m_gripAction[R],        "/user/hand/right/input/squeeze/click" },
    });

    XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &m_actionSet;
    if (!xrOk(xrAttachSessionActionSets(m_session, &attachInfo), "xrAttachSessionActionSets"))
        return false;

    // Pose action spaces (created after attach): identity offset, located against the reference space each frame.
    for (uint32 hand = 0; hand < HAND_COUNT; ++hand)
    {
        XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = m_poseAction[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f; // identity
        if (!xrOk(xrCreateActionSpace(m_session, &spaceInfo, &m_poseSpace[hand]), "xrCreateActionSpace"))
            return false;
    }

    // VIEW reference space for the head pose (separate from the renderer's own; used for head-relative locomotion).
    XrReferenceSpaceCreateInfo viewSpaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace.orientation.w = 1.0f; // identity
    xrOk(xrCreateReferenceSpace(m_session, &viewSpaceInfo, &m_viewSpace), "xrCreateReferenceSpace(VIEW)");

    m_enabled = true;
    printf("VrInput: action set attached (thumbsticks, poses, triggers, buttons)\n");
    return true;
}

void VrInput::update()
{
    m_active = false;
    m_moveAxis = glm::vec2(0.0f);
    m_turnAxis = glm::vec2(0.0f);
    for (VrHandState& hand : m_hands)
        hand = VrHandState{};
    m_headPoseValid = false;
    if (!m_enabled)
        return;

    XrActiveActionSet activeSet{ m_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    const XrResult sync = xrSyncActions(m_session, &syncInfo);
    if (sync == XR_SESSION_NOT_FOCUSED || !XR_SUCCEEDED(sync))
        return; // not focused yet (or lost): leave everything zeroed/inactive

    auto readVec2 = [&](XrAction action, glm::vec2& out) -> bool
    {
        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = action;
        XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &getInfo, &state)) && state.isActive)
        {
            out = glm::vec2(state.currentState.x, state.currentState.y);
            return true;
        }
        return false;
    };
    auto readFloat = [&](XrAction action) -> float
    {
        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = action;
        XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &state)) && state.isActive)
            return state.currentState;
        return 0.0f;
    };
    auto readBool = [&](XrAction action) -> bool
    {
        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = action;
        XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &state)) && state.isActive)
            return state.currentState == XR_TRUE;
        return false;
    };

    bool stickActive = false;
    stickActive |= readVec2(m_moveAction, m_moveAxis);
    stickActive |= readVec2(m_turnAction, m_turnAxis);
    m_active = stickActive;

    const XrTime displayTime = (XrTime)m_vrSession->predictedDisplayTime();

    if (m_viewSpace != XR_NULL_HANDLE && displayTime > 0)
    {
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xrLocateSpace(m_viewSpace, m_referenceSpace, displayTime, &loc))
            && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
            && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        {
            m_headPoseValid = true;
            m_headPosition = glm::vec3(loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
            m_headOrientation = glm::quat(loc.pose.orientation.w, loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z);
        }
    }

    for (uint32 hand = 0; hand < HAND_COUNT; ++hand)
    {
        VrHandState& state = m_hands[hand];
        state.trigger = readFloat(m_triggerAction[hand]);
        state.grip = readFloat(m_gripAction[hand]);
        state.primaryButton = readBool(m_primaryAction[hand]);
        state.secondaryButton = readBool(m_secondaryAction[hand]);

        if (displayTime > 0)
        {
            XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(xrLocateSpace(m_poseSpace[hand], m_referenceSpace, displayTime, &loc))
                && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
                && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            {
                state.poseValid = true;
                state.position = glm::vec3(loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
                state.orientation = glm::quat(loc.pose.orientation.w, loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z);
            }
        }
    }
}

void VrInput::destroy()
{
    for (uint32 hand = 0; hand < HAND_COUNT; ++hand)
    {
        if (m_poseSpace[hand] != XR_NULL_HANDLE)
        {
            xrDestroySpace(m_poseSpace[hand]);
            m_poseSpace[hand] = XR_NULL_HANDLE;
        }
    }
    if (m_viewSpace != XR_NULL_HANDLE)
    {
        xrDestroySpace(m_viewSpace);
        m_viewSpace = XR_NULL_HANDLE;
    }
    if (m_actionSet != XR_NULL_HANDLE)
    {
        xrDestroyActionSet(m_actionSet); // also destroys its child actions
        m_actionSet = XR_NULL_HANDLE;
    }
    m_enabled = false;
}

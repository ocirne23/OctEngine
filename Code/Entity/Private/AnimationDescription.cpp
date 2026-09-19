module Entity;

import Core;
import Core.glm;
import File;
import Animation;

import :AnimationDescription;

static char animLower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

static bool animIEquals(oc::string_view a, oc::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (animLower(a[i]) != animLower(b[i]))
            return false;
    return true;
}

// `Procedural [Walk]`: angles in degrees, phases in 0..1 periods. The Walk preset fills the four limb
// swings (bone names default to LeftLeg / RightLeg / LeftArm / RightArm); Swing / Bob lines add to either.
static void parseProcedural(const AssetNode& node, ProceduralClipDesc& out)
{
    glm::vec3 axis(0.0f, 0.0f, 1.0f);
    if (const AssetNode* n = node.find("Axis")) axis = n->asVec3(axis);

    if (animIEquals(node.asString(0), "Walk"))
    {
        WalkCycleParams walk;
        walk.axis = axis;
        if (const AssetNode* n = node.find("LegAngle"))  walk.legAngle = glm::radians(n->asFloat());
        if (const AssetNode* n = node.find("ArmAngle"))  walk.armAngle = glm::radians(n->asFloat());
        if (const AssetNode* n = node.find("LeftLeg"))   walk.leftLeg = n->asString();
        if (const AssetNode* n = node.find("RightLeg"))  walk.rightLeg = n->asString();
        if (const AssetNode* n = node.find("LeftArm"))   walk.leftArm = n->asString();
        if (const AssetNode* n = node.find("RightArm"))  walk.rightArm = n->asString();
        if (const AssetNode* n = node.find("BobBone"))   walk.bobBone = n->asString();
        if (const AssetNode* n = node.find("BobHeight")) walk.bobHeight = n->asFloat();
        out = makeWalkCycleDesc(walk);
    }
    if (const AssetNode* n = node.find("Duration")) out.duration = n->asFloat(0, out.duration);

    for (const AssetNode* s : node.findAll("Swing")) // Swing <bone> <angleDeg> [phase] [cycles], child Axis
    {
        ProceduralSwing swing;
        swing.bone = s->asString(0);
        swing.angle = glm::radians(s->asFloat(1, 30.0f));
        swing.phase = s->asFloat(2, 0.0f);
        swing.cycles = s->asFloat(3, 1.0f);
        swing.axis = axis;
        if (const AssetNode* n = s->find("Axis")) swing.axis = n->asVec3(axis);
        out.swings.push_back(oc::move(swing));
    }
    for (const AssetNode* b : node.findAll("Bob")) // Bob <bone> <height> [phase] [cycles], child Direction
    {
        ProceduralBob bob;
        bob.bone = b->asString(0);
        const float height = b->asFloat(1, 0.1f);
        bob.phase = b->asFloat(2, 0.0f);
        bob.cycles = b->asFloat(3, 2.0f);
        glm::vec3 dir(0.0f, 1.0f, 0.0f);
        if (const AssetNode* n = b->find("Direction")) dir = n->asVec3(dir);
        bob.offset = dir * height;
        out.bobs.push_back(oc::move(bob));
    }
}

bool toAnimationClipDesc(const AssetNode& node, AnimationClipDesc& out)
{
    if (!animIEquals(node.key, "Animation"))
        return false;

    out = AnimationClipDesc{};
    out.name = node.asString(0);
    if (const AssetNode* p = node.find("Procedural"))
    {
        out.isProcedural = true;
        parseProcedural(*p, out.procedural);
    }
    if (const AssetNode* src = node.find("ObjectContainer"))
    {
        out.source = src->asString();
        if (const AssetNode* t = src->find("Track")) // Track may sit under ObjectContainer ...
            out.track = t->asString();
    }
    if (const AssetNode* t = node.find("Track")) out.track = t->asString(); // ... or directly under Animation
    if (const AssetNode* l = node.find("Loop")) out.loop = l->asBool(0, true);
    if (const AssetNode* s = node.find("Skip")) out.skip = s->asString();
    for (const AssetNode* e : node.findAll("Event"))
        out.events.emplace_back(e->asString(0), e->asFloat(1)); // name, normalized time
    return true;
}

static AnimatorDesc::ParamType parseParamType(oc::string_view s)
{
    if (animIEquals(s, "Bool"))    return AnimatorDesc::ParamType::Bool;
    if (animIEquals(s, "Trigger")) return AnimatorDesc::ParamType::Trigger;
    return AnimatorDesc::ParamType::Float;
}

static AnimatorDesc::Condition::Op parseConditionOp(oc::string_view s)
{
    using Op = AnimatorDesc::Condition::Op;
    if (animIEquals(s, "Less"))    return Op::Less;
    if (animIEquals(s, "Equal"))   return Op::Equal;
    if (animIEquals(s, "Trigger")) return Op::Trigger;
    return Op::Greater;
}

static void parseSpeedBinding(const AssetNode& node, AnimatorDesc::SpeedBinding& b)
{
    if (const AssetNode* p = node.find("SpeedParam")) b.param = p->asString();
    if (const AssetNode* s = node.find("SpeedScale")) b.scale = s->asFloat(0, 1.0f);
    if (const AssetNode* c = node.find("Speed")) { b.hasConst = true; b.value = c->asFloat(0, 1.0f); }
}

static void parseStateMachine(const AssetNode& smNode, AnimatorDesc::StateMachine& sm)
{
    sm.present = true;
    for (const AssetNode& child : smNode.children)
    {
        if (animIEquals(child.key, "Entry"))
        {
            sm.entry = child.asString(0);
        }
        else if (animIEquals(child.key, "State"))
        {
            AnimatorDesc::State state;
            state.name = child.asString(0);
            if (const AssetNode* p = child.find("Play"))
                state.play = p->asString();
            parseSpeedBinding(child, state.speed);
            sm.states.push_back(oc::move(state));
        }
        else if (animIEquals(child.key, "Transition") || animIEquals(child.key, "AnyTransition"))
        {
            const bool any = animIEquals(child.key, "AnyTransition");
            AnimatorDesc::Transition tr;
            if (any)
            {
                tr.to = child.asString(0);
            }
            else
            {
                tr.from = child.asString(0);
                tr.to = child.asString(1);
            }
            for (const AssetNode* c : child.findAll("Condition"))
            {
                AnimatorDesc::Condition cond;
                cond.param = c->asString(0);
                cond.op = parseConditionOp(c->asString(1));
                if (cond.op == AnimatorDesc::Condition::Op::Equal)
                    cond.boolValue = c->asBool(2);
                else
                    cond.value = c->asFloat(2);
                tr.conditions.push_back(oc::move(cond));
            }
            if (const AssetNode* f = child.find("Fade"))     tr.fade = f->asFloat();
            if (const AssetNode* e = child.find("ExitTime")) tr.exitTime = e->asFloat();
            sm.transitions.push_back(oc::move(tr));
        }
    }
}

bool toAnimatorDesc(const AssetNode& node, AnimatorDesc& out)
{
    if (!animIEquals(node.key, "Animator"))
        return false;

    out = AnimatorDesc{};
    out.name = node.asString(0);

    for (const AssetNode& child : node.children)
    {
        if (animIEquals(child.key, "Parameter"))
        {
            AnimatorDesc::Param p;
            p.name = child.asString(0);
            p.type = parseParamType(child.asString(1));
            if (p.type == AnimatorDesc::ParamType::Bool)
                p.boolValue = child.asBool(2);
            else if (p.type == AnimatorDesc::ParamType::Float)
                p.floatValue = child.asFloat(2);
            out.params.push_back(oc::move(p));
        }
        else if (animIEquals(child.key, "Clip"))
        {
            // `Clip <localName> Anim <anmName>`; the "Anim" keyword is optional.
            AnimatorDesc::ClipRef ref;
            ref.localName = child.asString(0);
            ref.anmName = (child.numValues() >= 3 && animIEquals(child.asString(1), "Anim"))
                ? child.asString(2)
                : child.asString(1);
            out.clips.push_back(oc::move(ref));
        }
        else if (animIEquals(child.key, "BlendSpace1D"))
        {
            AnimatorDesc::BlendSpace bs;
            bs.name = child.asString(0);
            bs.axisParam = child.asString(1);
            for (const AssetNode* s : child.findAll("Sample"))
                bs.samples.push_back({ s->asString(0), s->asFloat(1) });
            out.blendSpaces.push_back(oc::move(bs));
        }
        else if (animIEquals(child.key, "StateMachine"))
        {
            parseStateMachine(child, out.stateMachine);
        }
    }
    parseSpeedBinding(node, out.speed); // animator-wide playback speed default
    return true;
}

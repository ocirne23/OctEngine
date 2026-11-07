module Animation;

import Core;
import Core.glm;

// ---- The text form ----
//
// Clip 1
//     Duration 1.0333
//     Steps 31                the editor's key grid: the duration in this many equal steps
//     Loop true
//     PreviewRig character
//     Event footstep 0.25
//     Track "mixamorig:Hips"
//         Position
//             0 0 1.02 0              time (s), then x y z
//             0.5 0 1.04 0 Smooth     an optional Step / Smooth: how the key blends to the next one (default Linear)
//         Rotation
//             0 0 0 0 1               time, then the quaternion x y z w
//         Scale
//             0 1 1 1
//
// Indentation is for the reader only: a key line belongs to the last channel keyword, a channel to the last Track.

namespace
{
char lowerChar(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

bool keyIs(oc::string_view a, const char* b)
{
    const oc::string_view bv(b);
    if (a.size() != bv.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lowerChar(a[i]) != lowerChar(bv[i]))
            return false;
    return true;
}

// Splits one line into tokens: runs of non-blanks, or "quoted" (no escapes). A # outside quotes ends the line.
void tokenize(oc::string_view line, oc::small_vector<oc::string_view, 8>& out)
{
    out.clear();
    size_t i = 0;
    while (i < line.size())
    {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r')
        {
            ++i;
            continue;
        }
        if (c == '#')
            break;
        if (c == '"')
        {
            const size_t end = line.find('"', i + 1);
            const size_t stop = end == oc::string_view::npos ? line.size() : end;
            out.push_back(line.substr(i + 1, stop - i - 1));
            i = stop + 1;
            continue;
        }
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r' && line[j] != '#')
            ++j;
        out.push_back(line.substr(i, j - i));
        i = j;
    }
}

bool toFloat(oc::string_view s, float& out)
{
    const char* first = s.data();
    const char* last = s.data() + s.size();
    if (first != last && *first == '+')
        ++first;
    const std::from_chars_result r = std::from_chars(first, last, out);
    return r.ec == std::errc() && r.ptr == last;
}

bool isNumberStart(oc::string_view s)
{
    if (s.empty())
        return false;
    const char c = s[0];
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

bool parseInterp(oc::string_view s, KeyInterp& out)
{
    if (keyIs(s, "Linear")) { out = KeyInterp::Linear; return true; }
    if (keyIs(s, "Step"))   { out = KeyInterp::Step; return true; }
    if (keyIs(s, "Smooth")) { out = KeyInterp::Smooth; return true; }
    return false;
}

void appendFloat(oc::string& out, float v)
{
    if (v == 0.0f)
        v = 0.0f; // no "-0"
    char buf[32];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v); // shortest form that reads back exactly
    out.append(buf, r.ptr);
}

void appendInterp(oc::string& out, KeyInterp interp)
{
    if (interp == KeyInterp::Step)
        out += " Step";
    else if (interp == KeyInterp::Smooth)
        out += " Smooth";
}

bool needsQuotes(const oc::string& s)
{
    if (s.empty())
        return true;
    for (char c : s)
        if (c == ' ' || c == '\t' || c == '#' || c == '"')
            return true;
    return false;
}

void appendName(oc::string& out, const oc::string& name)
{
    if (needsQuotes(name))
    {
        out += '"';
        out += name;
        out += '"';
    }
    else
        out += name;
}

template <typename KeyT>
void sortAndMerge(oc::vector<KeyT>& keys, float minGap)
{
    if (keys.size() < 2)
        return;
    oc::stable_sort(keys.begin(), keys.end(), [](const KeyT& a, const KeyT& b) { return a.time < b.time; });
    oc::vector<KeyT> merged;
    merged.reserve(keys.size());
    for (const KeyT& key : keys)
    {
        if (!merged.empty() && key.time - merged.back().time < minGap)
            merged.back() = key; // the later one wins
        else
            merged.push_back(key);
    }
    keys = oc::move(merged);
}

float vecError(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b); }
float quatErrorDegrees(const glm::quat& a, const glm::quat& b)
{
    const float d = glm::min(glm::abs(glm::dot(a, b)), 1.0f);
    return glm::degrees(2.0f * glm::acos(d));
}

glm::vec3 lerpValue(const glm::vec3& a, const glm::vec3& b, float f) { return glm::mix(a, b, f); }
glm::quat lerpValue(const glm::quat& a, const glm::quat& b, float f) { return glm::normalize(glm::slerp(a, b, f)); }
float valueError(const glm::vec3& a, const glm::vec3& b) { return vecError(a, b); }
float valueError(const glm::quat& a, const glm::quat& b) { return quatErrorDegrees(a, b); }

template <typename KeyT>
uint32 reduceChannel(oc::vector<KeyT>& keys, float tolerance)
{
    const uint32 n = (uint32)keys.size();
    if (n < 2)
        return 0;

    // A channel that never leaves its first value keeps that one key.
    bool constant = true;
    for (uint32 k = 1; k < n && constant; ++k)
        constant = keys[k - 1].interp == KeyInterp::Linear && valueError(keys[k].value, keys[0].value) <= tolerance;
    if (constant)
    {
        keys.resize(1);
        return n - 1;
    }

    oc::vector<KeyT> kept;
    kept.reserve(n);
    kept.push_back(keys[0]);
    uint32 anchor = 0;
    for (uint32 j = 1; j + 1 < n; ++j)
    {
        bool drop = keys[anchor].interp == KeyInterp::Linear && keys[j].interp == KeyInterp::Linear;
        // Every key between the anchor and j+1 must sit on the line anchor -> j+1, not only j: the error must not creep along a run of drops.
        for (uint32 k = anchor + 1; drop && k <= j; ++k)
        {
            const float span = keys[j + 1].time - keys[anchor].time;
            const float f = span > 1e-8f ? (keys[k].time - keys[anchor].time) / span : 0.0f;
            drop = valueError(lerpValue(keys[anchor].value, keys[j + 1].value, f), keys[k].value) <= tolerance;
        }
        if (!drop)
        {
            kept.push_back(keys[j]);
            anchor = j;
        }
    }
    kept.push_back(keys[n - 1]);
    const uint32 removed = n - (uint32)kept.size();
    keys = oc::move(kept);
    return removed;
}
}

bool parseClipAsset(oc::string_view text, ClipAsset& out, oc::string& error)
{
    out = ClipAsset{};
    ClipTrack* track = nullptr;
    int channel = -1; // ClipChannel, -1 = none open
    bool sawHeader = false;
    uint32 lineNo = 0;
    oc::small_vector<oc::string_view, 8> tokens;

    auto fail = [&](const oc::string& what)
    {
        error = "line " + oc::to_string(lineNo) + ": " + what;
        return false;
    };

    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == oc::string_view::npos)
            end = text.size();
        const oc::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;

        tokenize(line, tokens);
        if (tokens.empty())
        {
            if (end == text.size())
                break;
            continue;
        }
        const oc::string_view key = tokens[0];

        if (isNumberStart(key))
        {
            if (!track || channel < 0)
                return fail("a key outside a Position / Rotation / Scale block");
            const uint32 numValues = channel == (int)ClipChannel::Rotation ? 5u : 4u;
            if (tokens.size() < numValues)
                return fail("a key needs " + oc::to_string(numValues) + " numbers");
            float v[5] = {};
            for (uint32 i = 0; i < numValues; ++i)
                if (!toFloat(tokens[i], v[i]))
                    return fail("'" + oc::string(tokens[i].data(), tokens[i].size()) + "' is not a number");
            KeyInterp interp = KeyInterp::Linear;
            if (tokens.size() > numValues && !parseInterp(tokens[numValues], interp))
                return fail("unknown interpolation '" + oc::string(tokens[numValues].data(), tokens[numValues].size()) + "'");
            if (channel == (int)ClipChannel::Position)
                track->positionKeys.push_back({ v[0], glm::vec3(v[1], v[2], v[3]), interp });
            else if (channel == (int)ClipChannel::Rotation)
                track->rotationKeys.push_back({ v[0], glm::normalize(glm::quat(v[4], v[1], v[2], v[3])), interp });
            else
                track->scaleKeys.push_back({ v[0], glm::vec3(v[1], v[2], v[3]), interp });
        }
        else if (keyIs(key, "Clip"))
            sawHeader = true;
        else if (keyIs(key, "Duration"))
        {
            if (tokens.size() < 2 || !toFloat(tokens[1], out.duration))
                return fail("Duration needs seconds");
        }
        else if (keyIs(key, "Steps"))
        {
            float n = 0.0f;
            if (tokens.size() < 2 || !toFloat(tokens[1], n) || n < 1.0f)
                return fail("Steps needs a step count of 1 or more");
            out.steps = (uint32)n;
        }
        else if (keyIs(key, "Loop"))
            out.loop = tokens.size() < 2 || keyIs(tokens[1], "true") || tokens[1] == "1";
        else if (keyIs(key, "PreviewRig"))
            out.previewRig = tokens.size() >= 2 ? oc::string(tokens[1].data(), tokens[1].size()) : oc::string();
        else if (keyIs(key, "Event"))
        {
            AnimationEventKey ev;
            if (tokens.size() < 3 || !toFloat(tokens[2], ev.normalizedTime))
                return fail("Event needs a name and a normalized time");
            ev.name = oc::string(tokens[1].data(), tokens[1].size());
            out.events.push_back(oc::move(ev));
        }
        else if (keyIs(key, "Track"))
        {
            if (tokens.size() < 2)
                return fail("Track needs a bone name");
            track = &out.tracks.emplace_back();
            track->bone = oc::string(tokens[1].data(), tokens[1].size());
            channel = -1;
        }
        else if (keyIs(key, "Position") || keyIs(key, "Rotation") || keyIs(key, "Scale"))
        {
            if (!track)
                return fail("a channel outside a Track");
            channel = keyIs(key, "Position") ? (int)ClipChannel::Position : keyIs(key, "Rotation") ? (int)ClipChannel::Rotation : (int)ClipChannel::Scale;
        }
        else
            return fail("unknown keyword '" + oc::string(key.data(), key.size()) + "'");

        if (end == text.size())
            break;
    }
    if (!sawHeader)
    {
        error = "not a clip: no 'Clip' header";
        return false;
    }
    out.duration = glm::max(out.duration, 0.0f);
    sortClipKeys(out);
    return true;
}

oc::string writeClipAsset(const ClipAsset& clip)
{
    oc::string out;
    out.reserve(64 + clip.numKeys() * 40);
    out += "# Animation clip, written by the Animation Editor. A key is its time in seconds, then the value (a rotation is x y z w);\n";
    out += "# Step / Smooth after it sets how it blends to the next key (default Linear).\n";
    out += "Clip 1\n\tDuration ";
    appendFloat(out, clip.duration);
    out += "\n\tSteps ";
    out += oc::to_string(clip.steps);
    out += clip.loop ? "\n\tLoop true\n" : "\n\tLoop false\n";
    if (!clip.previewRig.empty())
    {
        out += "\tPreviewRig ";
        appendName(out, clip.previewRig);
        out += '\n';
    }
    for (const AnimationEventKey& ev : clip.events)
    {
        out += "\tEvent ";
        appendName(out, ev.name);
        out += ' ';
        appendFloat(out, ev.normalizedTime);
        out += '\n';
    }
    for (const ClipTrack& track : clip.tracks)
    {
        if (track.empty())
            continue;
        out += "\tTrack ";
        appendName(out, track.bone);
        out += '\n';
        if (!track.positionKeys.empty())
        {
            out += "\t\tPosition\n";
            for (const PositionKey& k : track.positionKeys)
            {
                out += "\t\t\t";
                appendFloat(out, k.time); out += ' ';
                appendFloat(out, k.value.x); out += ' ';
                appendFloat(out, k.value.y); out += ' ';
                appendFloat(out, k.value.z);
                appendInterp(out, k.interp);
                out += '\n';
            }
        }
        if (!track.rotationKeys.empty())
        {
            out += "\t\tRotation\n";
            for (const RotationKey& k : track.rotationKeys)
            {
                out += "\t\t\t";
                appendFloat(out, k.time); out += ' ';
                appendFloat(out, k.value.x); out += ' ';
                appendFloat(out, k.value.y); out += ' ';
                appendFloat(out, k.value.z); out += ' ';
                appendFloat(out, k.value.w);
                appendInterp(out, k.interp);
                out += '\n';
            }
        }
        if (!track.scaleKeys.empty())
        {
            out += "\t\tScale\n";
            for (const ScaleKey& k : track.scaleKeys)
            {
                out += "\t\t\t";
                appendFloat(out, k.time); out += ' ';
                appendFloat(out, k.value.x); out += ' ';
                appendFloat(out, k.value.y); out += ' ';
                appendFloat(out, k.value.z);
                appendInterp(out, k.interp);
                out += '\n';
            }
        }
    }
    return out;
}

AnimationClip buildAnimationClip(const ClipAsset& asset, const Skeleton& skeleton, const oc::string& name)
{
    AnimationClip clip;
    clip.name = name;
    clip.duration = asset.duration;
    clip.loop = asset.loop;
    clip.events = asset.events;
    clip.channels.reserve(asset.tracks.size());
    for (const ClipTrack& track : asset.tracks)
    {
        const int32 bone = skeleton.findBone(track.bone);
        if (bone < 0 || track.empty())
            continue;
        AnimationChannel& channel = clip.channels.emplace_back();
        channel.boneIndex = bone;
        channel.positionKeys = track.positionKeys;
        channel.rotationKeys = track.rotationKeys;
        channel.scaleKeys = track.scaleKeys;
    }
    return clip;
}

uint32 reduceClipKeys(ClipAsset& clip, const ClipReduceTolerance& tolerance)
{
    uint32 removed = 0;
    for (ClipTrack& track : clip.tracks)
    {
        removed += reduceChannel(track.positionKeys, tolerance.position);
        removed += reduceChannel(track.rotationKeys, tolerance.rotationDegrees);
        removed += reduceChannel(track.scaleKeys, tolerance.scale);
    }
    return removed;
}

void sortClipKeys(ClipAsset& clip, float minGap)
{
    for (ClipTrack& track : clip.tracks)
    {
        sortAndMerge(track.positionKeys, minGap);
        sortAndMerge(track.rotationKeys, minGap);
        sortAndMerge(track.scaleKeys, minGap);
    }
}

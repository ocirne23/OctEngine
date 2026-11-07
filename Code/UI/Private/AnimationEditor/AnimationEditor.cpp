module UI;

import Core;
import Core.Log;
import Core.imgui;
import Core.glm;
import Animation;
import Entity;
import File;
import RendererVK;
import :AnimationEditor;

static constexpr size_t c_maxUndoSteps = 96;

static bool aeContains(const oc::string& haystack, const char* needle)
{
	if (needle[0] == '\0')
		return true;
	oc::string h = haystack, n = needle;
	for (char& c : h) c = (char)std::tolower((unsigned char)c);
	for (char& c : n) c = (char)std::tolower((unsigned char)c);
	return h.find(n) != oc::string::npos;
}

static bool aeHasExtension(const oc::string& path, const char* ext)
{
	oc::string e = FileSystem::extension(path);
	for (char& c : e) c = (char)std::tolower((unsigned char)c);
	return e == ext;
}

static bool aeIsModelFile(const oc::string& path)
{
	return aeHasExtension(path, ".fbx") || aeHasExtension(path, ".gltf") || aeHasExtension(path, ".glb") || aeHasExtension(path, ".dae");
}

// Relative to Assets/, the form the registry uses. A drag from the browser can carry an absolute one.
static oc::string aeAssetRelative(const oc::string& path)
{
	if (!FileSystem::isAbsolute(path))
		return FileSystem::normalize(path);
	const oc::string relative = FileSystem::relativePath(path, oc::string(), /*allowMainThread*/ true);
	return relative.empty() ? path : relative;
}

static void aeCopyString(char* dst, size_t size, const oc::string& src)
{
	const size_t n = oc::min(src.size(), size - 1);
	memcpy(dst, src.data(), n);
	dst[n] = '\0';
}

static void aeLabel(const char* label, float width = 92.0f)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ImGui::SameLine(width);
	ImGui::SetNextItemWidth(-FLT_MIN);
}

// The key vector of one channel. Position and Scale share the vec3 layout but are separate types: the callers are generic lambdas.
template <typename F>
static void aeWithKeys(ClipTrack& track, ClipChannel channel, F&& f)
{
	switch (channel)
	{
	case ClipChannel::Position: f(track.positionKeys); break;
	case ClipChannel::Rotation: f(track.rotationKeys); break;
	default:                    f(track.scaleKeys); break;
	}
}

template <typename F>
static void aeWithKeys(const ClipTrack& track, ClipChannel channel, F&& f)
{
	switch (channel)
	{
	case ClipChannel::Position: f(track.positionKeys); break;
	case ClipChannel::Rotation: f(track.rotationKeys); break;
	default:                    f(track.scaleKeys); break;
	}
}

static const char* aeInterpName(KeyInterp interp)
{
	switch (interp)
	{
	case KeyInterp::Step:   return "Step";
	case KeyInterp::Smooth: return "Smooth";
	default:                return "Linear";
	}
}

// ---- The document ----

oc::string AnimationEditor::documentName() const
{
	return m_path.empty() ? oc::string("Untitled") : FileSystem::stem(m_path);
}

void AnimationEditor::setDocument(ClipAsset&& clip, const oc::string& path)
{
	m_clip = oc::move(clip);
	m_hasDocument = true;
	m_path = path;
	if (!m_clip.previewRig.empty())
		setRig(m_clip.previewRig);
	else
		m_clip.previewRig = m_rigName; // a clip with no rig of its own keeps the one in use
	m_baselineText = writeClipAsset(m_clip);
	m_dirty = path.empty();
	m_editPending = false;
	m_committed = m_clip;
	m_undoStack.clear();
	m_redoStack.clear();
	++m_docVersion;
	clearKeySelection();
	clearOverrides();
	m_selectedEvent = -1;
	m_curveRows.clear();
	m_time = 0.0f;
	m_playing = false;
	m_frameAllPending = true;
	m_curveFitPending = true;
}

void AnimationEditor::closeDocument()
{
	m_hasDocument = false;
	m_clip = ClipAsset{};
	m_committed = ClipAsset{};
	m_path.clear();
	m_baselineText.clear();
	m_dirty = false;
	m_editPending = false;
	m_undoStack.clear();
	m_redoStack.clear();
	clearKeySelection();
	clearOverrides();
	m_playing = false;
	++m_docVersion;
}

void AnimationEditor::commitPendingEdit()
{
	if (!m_editPending || !m_hasDocument || ImGui::IsAnyItemActive() || m_drag != DragMode::None)
		return;
	m_editPending = false;
	if (m_undoStack.size() >= c_maxUndoSteps)
		m_undoStack.erase(m_undoStack.begin());
	m_undoStack.push_back(oc::move(m_committed));
	m_redoStack.clear();
	m_committed = m_clip;
	m_dirty = m_path.empty() || writeClipAsset(m_clip) != m_baselineText;
}

void AnimationEditor::undo()
{
	if (m_editPending)
		commitPendingEdit();
	if (m_undoStack.empty() || m_editPending)
		return;
	m_redoStack.push_back(oc::move(m_committed));
	m_committed = oc::move(m_undoStack.back());
	m_undoStack.pop_back();
	m_clip = m_committed;
	++m_docVersion;
	clearKeySelection();
	clearOverrides();
	m_dirty = m_path.empty() || writeClipAsset(m_clip) != m_baselineText;
}

void AnimationEditor::redo()
{
	if (m_redoStack.empty() || m_editPending)
		return;
	m_undoStack.push_back(oc::move(m_committed));
	m_committed = oc::move(m_redoStack.back());
	m_redoStack.pop_back();
	m_clip = m_committed;
	++m_docVersion;
	clearKeySelection();
	clearOverrides();
	m_dirty = m_path.empty() || writeClipAsset(m_clip) != m_baselineText;
}

// ---- Open, save (guarded) ----

void AnimationEditor::requestOpen(const oc::string& rawPath)
{
	if (aeIsModelFile(rawPath) || aeHasExtension(rawPath, ".anm"))
	{
		requestImport(rawPath);
		return;
	}
	if (!aeHasExtension(rawPath, ".clip"))
		return;
	m_pendingPath = aeAssetRelative(rawPath);
	guardedSwitch(PendingSwitch::Open);
}

void AnimationEditor::requestImport(const oc::string& rawPath)
{
	const oc::string path = aeAssetRelative(rawPath);
	m_importAnmPicked.clear();
	if (aeHasExtension(path, ".anm"))
	{
		// The .anm entries of that file, picked: one click converts them all.
		m_importFromAnm = true;
		AssetNode doc;
		oc::string error;
		if (loadAssetFile(path, doc, error))
			for (const AssetNode& decl : doc.children)
				if (decl.key == "Animation" && !decl.asString(0).empty())
					m_importAnmPicked.insert(decl.asString(0));
	}
	else
	{
		m_importFromAnm = false;
		m_importSource = path;
	}
	m_openImportPopup = true;
}

void AnimationEditor::guardedSwitch(PendingSwitch what)
{
	if (m_hasDocument && isDirty())
	{
		m_pendingSwitch = what;
		m_openUnsavedPopup = true;
		return;
	}
	runSwitch(what);
}

void AnimationEditor::runSwitch(PendingSwitch what)
{
	switch (what)
	{
	case PendingSwitch::New:   doNew(); break;
	case PendingSwitch::Open:  doOpen(m_pendingPath); break;
	case PendingSwitch::Close: closeDocument(); break;
	default: break;
	}
	m_pendingSwitch = PendingSwitch::None;
}

void AnimationEditor::doOpen(const oc::string& path)
{
	// The widget pass is a worker: reading here is allowed, and a clip needs no main-thread step to become a document.
	const oc::string text = FileSystem::readFileStr(path, /*allowMainThread*/ true);
	ClipAsset clip;
	oc::string error;
	if (text.empty() || !parseClipAsset(text, clip, error))
	{
		showMessage("Could not open '" + path + "': " + (text.empty() ? oc::string("the file is empty or missing") : error), true);
		return;
	}
	setDocument(oc::move(clip), path);
	showMessage("Opened " + path, false);
}

void AnimationEditor::doNew()
{
	ClipAsset clip;
	clip.duration = 1.0f;
	clip.steps = 10; // coarse: subdivide where the motion needs more keys
	clip.previewRig = m_rigName;
	setDocument(oc::move(clip), oc::string());
	aeCopyString(m_saveAsName, sizeof(m_saveAsName), oc::string("NewClip"));
}

void AnimationEditor::trySave(const oc::string& rawPath)
{
	if (!m_hasDocument || rawPath.empty())
		return;
	oc::string path = aeAssetRelative(rawPath);
	if (!aeHasExtension(path, ".clip"))
		path += ".clip";
	if (!FileSystem::pathEquals(path, m_path) && FileSystem::exists(path, /*allowMainThread*/ true))
	{
		m_pendingSavePath = path;
		m_openOverwritePopup = true;
		return;
	}
	queueSave(path);
}

void AnimationEditor::queueSave(const oc::string& path)
{
	commitPendingEdit();
	m_editPending = false;
	m_path = path;
	m_clip.previewRig = m_rigName;
	m_baselineText = writeClipAsset(m_clip);
	m_changes.push_back({ EntityChange::SaveAnimationClip{ path, m_baselineText } });
	m_dirty = false;
	m_committed = m_clip;
	m_clipFiles.clear();
	showMessage("Saved " + path, false);
}

// ---- Rig ----

void AnimationEditor::needContainer(const oc::string& name)
{
	if (name.empty() || !Globals::assetRegistry.findObjectContainer(name))
		return;
	if (oc::find(m_containerLoadRequests.begin(), m_containerLoadRequests.end(), name) == m_containerLoadRequests.end())
		m_containerLoadRequests.push_back(name);
}

void AnimationEditor::setRig(const oc::string& containerName)
{
	if (containerName == m_rigName)
		return;
	m_rigName = containerName;
	m_rigContainer = nullptr;
	m_skeleton = nullptr;
	m_selectedBone = -1;
	clearOverrides();
	m_framePending = true;
	if (m_hasDocument && m_clip.previewRig != containerName)
	{
		// The rig is a view setting of the file: it is saved with it, but it is no undo step.
		m_clip.previewRig = containerName;
		m_committed.previewRig = containerName;
	}
}

void AnimationEditor::resolveRig()
{
	if (m_rigName.empty())
	{
		m_rigContainer = nullptr;
		m_skeleton = nullptr;
		return;
	}
	ObjectContainer* container = Globals::world.findLoadedContainer(m_rigName); // lookup only: a miss is imported on main
	if (!container)
	{
		needContainer(m_rigName);
		m_rigContainer = nullptr;
		m_skeleton = nullptr;
		return;
	}
	m_rigContainer = container;
	m_skeleton = (container->isSkinned() && container->getSkeleton() && container->getSkeleton()->isValid()) ? container->getSkeleton() : nullptr;
}

void AnimationEditor::rebuildRuntimeClip()
{
	const Skeleton* skel = m_skeleton;
	if (!skel)
	{
		m_builtSkeleton = nullptr;
		return;
	}
	const bool skeletonChanged = skel != m_builtSkeleton;
	if (!skeletonChanged && m_builtVersion == m_docVersion)
		return;

	m_runtimeClip = buildAnimationClip(m_clip, *skel, "AnimationEditor");
	if (skeletonChanged)
	{
		m_player.initialize(skel, &m_runtimeClip);
		m_player.setPaused(true);
		const uint32 n = skel->numBones();
		m_boneIsHelper.assign(n, 0);
		for (uint32 i = 0; i < n; ++i)
			m_boneIsHelper[i] = (skel->parentIndices[i] < 0 || skel->boneNames[i].find("$AssimpFbx$") != oc::string::npos) ? 1 : 0;
	}
	const uint32 n = skel->numBones();
	m_boneTrack.assign(n, -1);
	m_boneSubtreeAnimated.assign(n, 0);
	for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
	{
		const int32 bone = skel->findBone(m_clip.tracks[t].bone);
		if (bone >= 0)
			m_boneTrack[bone] = (int32)t;
	}
	// Children come after their parents: a backward pass carries "animated below" up the tree.
	for (int32 i = (int32)n - 1; i >= 0; --i)
	{
		if (m_boneTrack[i] >= 0 && !m_clip.tracks[m_boneTrack[i]].empty())
			m_boneSubtreeAnimated[i] = 1;
		const int32 parent = skel->parentIndices[i];
		if (parent >= 0 && m_boneSubtreeAnimated[i])
			m_boneSubtreeAnimated[parent] = 1;
	}
	m_builtVersion = m_docVersion;
	m_builtSkeleton = skel;
}

// ---- Keys ----

uint64 AnimationEditor::keyId(uint32 track, ClipChannel channel, float time)
{
	return (uint64(track) << 34) | (uint64(channel) << 32) | uint64(oc::bitCast<uint32>(time));
}

AnimationEditor::KeyRef AnimationEditor::keyFromId(uint64 id)
{
	return KeyRef{ uint32(id >> 34), ClipChannel(uint32(id >> 32) & 3u), oc::bitCast<float>(uint32(id & 0xFFFFFFFFull)) };
}

void AnimationEditor::selectKey(uint32 track, ClipChannel channel, float time, bool select)
{
	const uint64 id = keyId(track, channel, time);
	if (select)
		m_keySelection.insert(id);
	else
		m_keySelection.erase(id);
}

void AnimationEditor::collectSelectedKeys(oc::vector<KeyRef>& out) const
{
	out.clear();
	out.reserve(m_keySelection.size());
	for (uint64 id : m_keySelection)
		out.push_back(keyFromId(id));
}

float AnimationEditor::snapTime(float t) const
{
	t = glm::max(t, 0.0f);
	if (!m_snap)
		return t;
	return float(stepIndex(t)) * gridStep();
}

void AnimationEditor::extendTo(float t)
{
	const float oldDuration = m_clip.duration;
	if (t <= oldDuration + gridStep() * 0.01f)
		return;
	const float step = gridStep();
	const uint32 extra = (uint32)glm::max(glm::ceil((t - oldDuration) / step - 0.01f), 1.0f);
	m_clip.steps += extra;
	m_clip.duration = float(m_clip.steps) * step;
	// Events are stored normalized: they keep their seconds, not their fraction.
	if (oldDuration > 1e-6f)
		for (AnimationEventKey& ev : m_clip.events)
			ev.normalizedTime *= oldDuration / m_clip.duration;
	markEdited();
}

float AnimationEditor::latestContent() const
{
	float latest = 0.0f;
	for (const ClipTrack& track : m_clip.tracks)
		for (uint32 c = 0; c < c_numClipChannels; ++c)
			aeWithKeys(track, ClipChannel(c), [&](const auto& keys) { if (!keys.empty()) latest = glm::max(latest, keys.back().time); });
	for (const AnimationEventKey& ev : m_clip.events)
		latest = glm::max(latest, ev.normalizedTime * m_clip.duration);
	return latest;
}

void AnimationEditor::shrinkToContent(float latestBefore)
{
	const float step = gridStep();
	const float end = m_clip.duration;
	const float eps = step * 0.25f;
	const float latest = latestContent();
	if (latestBefore < end - eps || latest >= end - eps)
		return;
	const uint32 steps = (uint32)glm::max(glm::ceil(latest / step - 0.01f), 1.0f);
	if (steps >= m_clip.steps)
		return;
	m_clip.steps = steps;
	m_clip.duration = float(steps) * step;
	// Events are stored normalized: they keep their seconds, not their fraction.
	for (AnimationEventKey& ev : m_clip.events)
		ev.normalizedTime = glm::clamp(ev.normalizedTime * end / m_clip.duration, 0.0f, 1.0f);
	m_time = glm::min(m_time, m_clip.duration);
	markEdited();
}

void AnimationEditor::setSteps(uint32 steps)
{
	steps = glm::clamp(steps, 1u, 100000u);
	if (steps == m_clip.steps)
		return;
	m_clip.steps = steps;
	markEdited();
}

int32 AnimationEditor::ensureTrack(const oc::string& bone)
{
	const int32 found = m_clip.findTrack(bone);
	if (found >= 0)
		return found;
	m_clip.tracks.emplace_back().bone = bone;
	return (int32)m_clip.tracks.size() - 1;
}

void AnimationEditor::setKeyVec3(uint32 track, ClipChannel channel, float time, const glm::vec3& value)
{
	const float eps = gridStep() * 0.25f;
	const KeyInterp interp = m_newKeyInterp;
	auto apply = [&](auto& keys)
	{
		using KeyT = typename std::remove_reference_t<decltype(keys)>::value_type;
		if constexpr (!std::is_same_v<KeyT, RotationKey>)
		{
			for (KeyT& k : keys)
				if (glm::abs(k.time - time) <= eps)
				{
					k.value = value;
					return;
				}
			auto it = keys.begin();
			while (it != keys.end() && it->time < time)
				++it;
			keys.insert(it, KeyT{ time, value, interp });
		}
	};
	aeWithKeys(m_clip.tracks[track], channel, apply);
	extendTo(time);
	markEdited();
}

void AnimationEditor::setKeyQuat(uint32 track, float time, const glm::quat& value)
{
	const float eps = gridStep() * 0.25f;
	oc::vector<RotationKey>& keys = m_clip.tracks[track].rotationKeys;
	for (RotationKey& k : keys)
		if (glm::abs(k.time - time) <= eps)
		{
			k.value = glm::normalize(value);
			markEdited();
			return;
		}
	auto it = keys.begin();
	while (it != keys.end() && it->time < time)
		++it;
	keys.insert(it, RotationKey{ time, glm::normalize(value), m_newKeyInterp });
	extendTo(time);
	markEdited();
}

void AnimationEditor::deleteSelectedKeys()
{
	if (m_keySelection.empty())
		return;
	const float latestBefore = latestContent();
	for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
		for (uint32 c = 0; c < c_numClipChannels; ++c)
		{
			const ClipChannel channel = ClipChannel(c);
			aeWithKeys(m_clip.tracks[t], channel, [&](auto& keys)
			{
				keys.erase(oc::remove_if(keys.begin(), keys.end(), [&](const auto& k) { return isKeySelected(t, channel, k.time); }), keys.end());
			});
		}
	clearKeySelection();
	shrinkToContent(latestBefore);
	markEdited();
}

void AnimationEditor::deleteEvent(int32 index)
{
	if (index < 0 || index >= (int32)m_clip.events.size())
		return;
	const float latestBefore = latestContent();
	m_clip.events.erase(m_clip.events.begin() + index);
	m_selectedEvent = -1;
	shrinkToContent(latestBefore);
	markEdited();
}

void AnimationEditor::deleteTrack(int32 track)
{
	if (track < 0 || track >= (int32)m_clip.tracks.size())
		return;
	const float latestBefore = latestContent();
	m_clip.tracks.erase(m_clip.tracks.begin() + track);
	clearKeySelection();
	m_curveRows.clear();
	if (m_selectedTrack == track)
		m_selectedTrack = -1;
	shrinkToContent(latestBefore);
	markEdited();
}

void AnimationEditor::moveSelectedKeys(float delta)
{
	if (m_keySelection.empty() || delta == 0.0f)
		return;
	const float eps = gridStep() * 0.25f;
	const float latestBefore = latestContent();
	oc::unordered_set<uint64> moved;
	float latest = 0.0f;
	for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
		for (uint32 c = 0; c < c_numClipChannels; ++c)
		{
			const ClipChannel channel = ClipChannel(c);
			aeWithKeys(m_clip.tracks[t], channel, [&](auto& keys)
			{
				using KeyT = typename std::remove_reference_t<decltype(keys)>::value_type;
				oc::vector<KeyT> picked;
				oc::vector<KeyT> rest;
				for (const KeyT& k : keys)
					(isKeySelected(t, channel, k.time) ? picked : rest).push_back(k);
				if (picked.empty())
					return;
				for (KeyT& k : picked)
				{
					k.time = glm::max(k.time + delta, 0.0f);
					latest = glm::max(latest, k.time);
					moved.insert(keyId(t, channel, k.time));
				}
				// A moved key replaces an unmoved one it lands on.
				rest.erase(oc::remove_if(rest.begin(), rest.end(), [&](const KeyT& r)
				{
					for (const KeyT& p : picked)
						if (glm::abs(p.time - r.time) <= eps)
							return true;
					return false;
				}), rest.end());
				rest.insert(rest.end(), picked.begin(), picked.end());
				oc::stable_sort(rest.begin(), rest.end(), [](const KeyT& a, const KeyT& b) { return a.time < b.time; });
				keys = oc::move(rest);
			});
		}
	m_keySelection = oc::move(moved);
	extendTo(latest);
	shrinkToContent(latestBefore);
	markEdited();
}

void AnimationEditor::setSelectedKeysInterp(KeyInterp interp)
{
	if (m_keySelection.empty())
		return;
	for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
		for (uint32 c = 0; c < c_numClipChannels; ++c)
		{
			const ClipChannel channel = ClipChannel(c);
			aeWithKeys(m_clip.tracks[t], channel, [&](auto& keys)
			{
				for (auto& k : keys)
					if (isKeySelected(t, channel, k.time))
						k.interp = interp;
			});
		}
	markEdited();
}

void AnimationEditor::copySelectedKeys()
{
	if (m_keySelection.empty())
		return;
	m_clipboard.clear();
	float earliest = FLT_MAX;
	for (uint64 id : m_keySelection)
		earliest = glm::min(earliest, keyFromId(id).time);
	for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
		for (uint32 c = 0; c < c_numClipChannels; ++c)
		{
			const ClipChannel channel = ClipChannel(c);
			aeWithKeys(m_clip.tracks[t], channel, [&](const auto& keys)
			{
				for (const auto& k : keys)
				{
					if (!isKeySelected(t, channel, k.time))
						continue;
					ClipboardKey ck{ m_clip.tracks[t].bone, channel, k.time - earliest, glm::vec4(0.0f), k.interp };
					using KeyT = std::remove_cvref_t<decltype(k)>;
					if constexpr (std::is_same_v<KeyT, RotationKey>)
						ck.value = glm::vec4(k.value.x, k.value.y, k.value.z, k.value.w);
					else
						ck.value = glm::vec4(k.value, 0.0f);
					m_clipboard.push_back(oc::move(ck));
				}
			});
		}
	showMessage(oc::to_string((uint32)m_clipboard.size()) + " keys copied", false);
}

void AnimationEditor::pasteKeys(float atTime)
{
	if (m_clipboard.empty() || !m_hasDocument)
		return;
	const KeyInterp keep = m_newKeyInterp;
	clearKeySelection();
	for (const ClipboardKey& ck : m_clipboard)
	{
		const int32 track = ensureTrack(ck.bone);
		const float t = snapTime(atTime + ck.offset);
		m_newKeyInterp = ck.interp;
		if (ck.channel == ClipChannel::Rotation)
			setKeyQuat((uint32)track, t, glm::quat(ck.value.w, ck.value.x, ck.value.y, ck.value.z));
		else
			setKeyVec3((uint32)track, ck.channel, t, glm::vec3(ck.value));
		// The stored time may be an existing key's: select what is there now.
		aeWithKeys(m_clip.tracks[track], ck.channel, [&](const auto& keys)
		{
			for (const auto& k : keys)
				if (glm::abs(k.time - t) <= gridStep() * 0.25f)
					selectKey((uint32)track, ck.channel, k.time, true);
		});
	}
	m_newKeyInterp = keep;
	markEdited();
}

void AnimationEditor::currentLocalPose(int32 bone, glm::vec3& pos, glm::quat& rot, glm::vec3& scale) const
{
	m_player.getSampledLocalPose((uint32)bone, pos, rot, scale);
	if (const auto it = m_overrides.find(bone); it != m_overrides.end())
	{
		const PoseOverride& o = it->second;
		if (o.channels & (1u << (uint32)ClipChannel::Position)) pos = o.pos;
		if (o.channels & (1u << (uint32)ClipChannel::Rotation)) rot = o.rot;
		if (o.channels & (1u << (uint32)ClipChannel::Scale))    scale = o.scale;
	}
}

void AnimationEditor::keyBone(int32 bone, bool position, bool rotation, bool scale)
{
	if (!m_skeleton || bone < 0 || (uint32)bone >= m_skeleton->numBones() || !m_hasDocument)
		return;
	glm::vec3 p, s;
	glm::quat r;
	currentLocalPose(bone, p, r, s);
	const uint32 track = (uint32)ensureTrack(m_skeleton->boneNames[bone]);
	const float t = snapTime(m_time);
	if (position) setKeyVec3(track, ClipChannel::Position, t, p);
	if (rotation) setKeyQuat(track, t, r);
	if (scale)    setKeyVec3(track, ClipChannel::Scale, t, s);
	m_overrides.erase(bone);
}

void AnimationEditor::keySelectedBones()
{
	if (!m_skeleton || m_selectedBone < 0)
	{
		showMessage("Select a bone to key (click a joint in the preview, or a row)", true);
		return;
	}
	// Scale only when the bone has scale keys already or is scaled: most rigs never animate it.
	glm::vec3 p, s;
	glm::quat r;
	currentLocalPose(m_selectedBone, p, r, s);
	const int32 track = m_clip.findTrack(m_skeleton->boneNames[m_selectedBone]);
	const bool scale = (track >= 0 && !m_clip.tracks[track].scaleKeys.empty()) || glm::length(s - glm::vec3(1.0f)) > 1e-4f;
	keyBone(m_selectedBone, true, true, scale);
}

void AnimationEditor::editBonePose(int32 bone, ClipChannel channel, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scale)
{
	if (!m_skeleton || bone < 0 || !m_hasDocument)
		return;
	if (m_recording)
	{
		const uint32 track = (uint32)ensureTrack(m_skeleton->boneNames[bone]);
		const float t = snapTime(m_time);
		if (channel == ClipChannel::Rotation)
			setKeyQuat(track, t, rot);
		else
			setKeyVec3(track, channel, t, channel == ClipChannel::Position ? pos : scale);
		if (auto it = m_overrides.find(bone); it != m_overrides.end())
			it->second.channels &= uint8(~(1u << (uint32)channel));
		return;
	}
	PoseOverride& o = m_overrides[bone];
	if (o.channels == 0)
		currentLocalPose(bone, o.pos, o.rot, o.scale);
	o.pos = pos;
	o.rot = rot;
	o.scale = scale;
	o.channels |= uint8(1u << (uint32)channel);
}

// ---- Clip operations ----

void AnimationEditor::setClipDuration(float seconds, bool scaleKeys)
{
	const float oldDuration = m_clip.duration;
	if (scaleKeys && oldDuration > 1e-6f)
	{
		// The keys stay on their steps: the step count holds, the step scales. Events are normalized, so they follow by themselves.
		seconds = glm::max(seconds, 1e-3f);
		const float s = seconds / oldDuration;
		for (ClipTrack& track : m_clip.tracks)
			for (uint32 c = 0; c < c_numClipChannels; ++c)
				aeWithKeys(track, ClipChannel(c), [&](auto& keys) { for (auto& k : keys) k.time *= s; });
		clearKeySelection();
		m_clip.duration = seconds;
		m_time *= s;
	}
	else
	{
		// The step holds: the clip is trimmed or extended by whole steps, and the events keep their seconds.
		const float step = gridStep();
		m_clip.steps = (uint32)glm::max(glm::round(seconds / step), 1.0f);
		m_clip.duration = float(m_clip.steps) * step;
		if (oldDuration > 1e-6f)
			for (AnimationEventKey& ev : m_clip.events)
				ev.normalizedTime = glm::clamp(ev.normalizedTime * oldDuration / m_clip.duration, 0.0f, 1.0f);
		m_time = glm::min(m_time, m_clip.duration);
	}
	markEdited();
}

void AnimationEditor::reduceKeys()
{
	const uint32 before = m_clip.numKeys();
	const uint32 removed = reduceClipKeys(m_clip, m_tolerance);
	clearKeySelection();
	markEdited();
	showMessage("Reduced " + oc::to_string(before) + " keys to " + oc::to_string(before - removed), false);
}

void AnimationEditor::matchLoopPose()
{
	// The last frame takes the first frame's pose on every channel, so the loop has no pop.
	const float end = m_clip.duration;
	for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
	{
		ClipTrack& track = m_clip.tracks[t];
		if (!track.positionKeys.empty())
			setKeyVec3(t, ClipChannel::Position, end, AnimKeys::sampleVec3(oc::span<const PositionKey>(track.positionKeys), 0.0f, glm::vec3(0.0f)));
		if (!track.rotationKeys.empty())
			setKeyQuat(t, end, AnimKeys::sampleQuat(oc::span<const RotationKey>(track.rotationKeys), 0.0f, glm::quat(1, 0, 0, 0)));
		if (!track.scaleKeys.empty())
			setKeyVec3(t, ClipChannel::Scale, end, AnimKeys::sampleVec3(oc::span<const ScaleKey>(track.scaleKeys), 0.0f, glm::vec3(1.0f)));
	}
	markEdited();
}

void AnimationEditor::removeUnboundTracks()
{
	if (!m_skeleton)
		return;
	const size_t before = m_clip.tracks.size();
	const float latestBefore = latestContent();
	m_clip.tracks.erase(oc::remove_if(m_clip.tracks.begin(), m_clip.tracks.end(),
		[&](const ClipTrack& t) { return m_skeleton->findBone(t.bone) < 0; }), m_clip.tracks.end());
	clearKeySelection();
	m_selectedTrack = -1;
	shrinkToContent(latestBefore);
	markEdited();
	showMessage("Removed " + oc::to_string(uint32(before - m_clip.tracks.size())) + " tracks the rig has no bone for", false);
}

// ---- Playback ----

void AnimationEditor::setTime(float t)
{
	// The playhead may go past the end: the clip grows to there (a click beyond the last step is how a clip gets longer).
	t = glm::max(t, 0.0f);
	if (m_hasDocument)
		extendTo(t);
	if (t != m_time && !m_recording)
		clearOverrides(); // an unkeyed edit is gone once the playhead leaves its step
	m_time = t;
}

void AnimationEditor::stepFrames(int32 steps)
{
	m_playing = false;
	setTime(float(glm::max(stepIndex(m_time) + steps, 0)) * gridStep());
}

void AnimationEditor::jumpKey(int32 direction)
{
	m_playing = false;
	const float eps = gridStep() * 0.25f;
	float best = direction > 0 ? FLT_MAX : -FLT_MAX;
	auto consider = [&](float t)
	{
		if (direction > 0 && t > m_time + eps && t < best) best = t;
		if (direction < 0 && t < m_time - eps && t > best) best = t;
	};
	const int32 onlyTrack = (m_skeleton && m_selectedBone >= 0 && (uint32)m_selectedBone < m_boneTrack.size()) ? m_boneTrack[m_selectedBone] : m_selectedTrack;
	for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
	{
		if (onlyTrack >= 0 && (int32)t != onlyTrack)
			continue;
		for (uint32 c = 0; c < c_numClipChannels; ++c)
			aeWithKeys(m_clip.tracks[t], ClipChannel(c), [&](const auto& keys) { for (const auto& k : keys) consider(k.time); });
	}
	if (best != FLT_MAX && best != -FLT_MAX)
		setTime(best);
}

void AnimationEditor::tickPlayback(float dt)
{
	if (!m_playing)
		return;
	const float duration = m_clip.duration;
	if (duration <= 1e-6f)
	{
		m_playing = false;
		return;
	}
	clearOverrides();
	float t = m_time + dt * m_playSpeed;
	if (t > duration || t < 0.0f)
	{
		if (m_clip.loop)
			t = t - duration * glm::floor(t / duration);
		else
		{
			t = glm::clamp(t, 0.0f, duration);
			m_playing = false;
		}
	}
	m_time = t;
}

// ---- Main-thread work ----

void AnimationEditor::flushMainThreadWork()
{
	for (const oc::string& name : m_containerLoadRequests)
		Globals::world.getOrLoadContainer(name);
	m_containerLoadRequests.clear();

	for (ImportJob& job : m_importJobs)
	{
		oc::vector<ImportedClip> imported;
		oc::string error;
		{
			const FileSystem::AllowMainThreadIO allowIo; // an explicit editor import
			if (!ISceneData::importClipAssets(job.source.c_str(), imported, job.skip.empty() ? nullptr : job.skip.c_str(),
				job.track.empty() ? nullptr : job.track.c_str(), &error))
			{
				showMessage("Import '" + job.name + "' failed: " + error, true);
				Log::warning("Animation Editor: import of '" + job.source + "' failed: " + error);
				continue;
			}
			FileSystem::createDirectories(job.folder);
		}
		for (uint32 i = 0; i < (uint32)imported.size(); ++i)
		{
			ClipAsset& clip = imported[i].clip;
			clip.previewRig = job.rig;
			if (job.fromAnm)
			{
				clip.loop = job.loop;
				clip.events = job.events;
			}
			if (job.reduce)
				reduceClipKeys(clip, job.tolerance);
			const oc::string name = imported.size() > 1 ? job.name + "_" + oc::to_string(i) : job.name;
			const oc::string path = FileSystem::join(job.folder, name + ".clip");
			oc::string text = writeClipAsset(clip);
			m_changes.push_back({ EntityChange::SaveAnimationClip{ path, text } });
			Log::info("Animation Editor: imported '" + job.source + "' track '" + imported[i].trackName + "' -> " + path
				+ " (" + oc::to_string(clip.numKeys()) + " keys)");
			if (i == 0 && job.openAfter && !(m_hasDocument && isDirty()))
			{
				setDocument(oc::move(clip), path);
				m_baselineText = oc::move(text);
				m_dirty = false;
			}
		}
		showMessage("Imported '" + job.name + "' (" + oc::to_string((uint32)imported.size()) + " clip" + (imported.size() > 1 ? "s)" : ")"), false);
	}
	m_importJobs.clear();
}

// ---- Shortcuts ----

void AnimationEditor::handleShortcuts()
{
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
		return;
	const ImGuiIO& io = ImGui::GetIO();
	if (io.WantTextInput)
		return;
	const bool ctrl = io.KeyCtrl;
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
	{
		if (m_path.empty())
			m_openSaveAsPopup = true;
		else
			trySave(m_path);
	}
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
		m_openOpenPopup = true;
	if (!m_hasDocument)
		return;
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, true))
		io.KeyShift ? redo() : undo();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, true))
		redo();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
		copySelectedKeys();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
		pasteKeys(m_time);
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
	{
		clearKeySelection();
		for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
			for (uint32 c = 0; c < c_numClipChannels; ++c)
				aeWithKeys(m_clip.tracks[t], ClipChannel(c), [&](const auto& keys) { for (const auto& k : keys) selectKey(t, ClipChannel(c), k.time, true); });
	}
	if (ctrl)
		return;
	if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::IsAnyItemFocused()) // a nav-focused button takes Space itself
		m_playing = !m_playing;
	if (ImGui::IsKeyPressed(ImGuiKey_K, false))
		keySelectedBones();
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		if (!m_keySelection.empty())
			deleteSelectedKeys();
		else
			deleteEvent(m_selectedEvent);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Comma, true))
		io.KeyAlt ? jumpKey(-1) : stepFrames(-1);
	if (ImGui::IsKeyPressed(ImGuiKey_Period, true))
		io.KeyAlt ? jumpKey(1) : stepFrames(1);
	if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
		setTime(0.0f);
	if (ImGui::IsKeyPressed(ImGuiKey_End, false))
		setTime(m_clip.duration);
	if (ImGui::IsKeyPressed(ImGuiKey_W, false))
		m_gizmoMode = GizmoMode::Move;
	if (ImGui::IsKeyPressed(ImGuiKey_E, false))
		m_gizmoMode = GizmoMode::Rotate;
	if (ImGui::IsKeyPressed(ImGuiKey_R, false))
		m_recording = !m_recording;
}

// ---- The panel ----

void AnimationEditor::render(double deltaSec)
{
	if (m_messageTimer > 0.0f)
		m_messageTimer -= (float)deltaSec;

	handleShortcuts();
	renderToolbar();
	renderPopups();

	if (!m_hasDocument)
	{
		renderEmptyState();
		return;
	}

	resolveRig();
	rebuildRuntimeClip();
	tickPlayback((float)deltaSec);

	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float splitter = 5.0f;
	const float topHeight = glm::clamp(glm::floor(avail.y * m_topFraction), 120.0f, glm::max(avail.y - 140.0f, 120.0f));
	const float inspectorWidth = glm::clamp(m_inspectorWidth, 200.0f, glm::max(avail.x - 220.0f, 200.0f));
	const float previewWidth = glm::max(avail.x - inspectorWidth - splitter, 50.0f);

	// ---- Top: the preview | the inspector ----
	if (ImGui::BeginChild("##ae_preview", ImVec2(previewWidth, topHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		renderPreview(previewWidth, topHeight);
	ImGui::EndChild();
	ImGui::SameLine(0.0f, 0.0f);
	ImGui::InvisibleButton("##ae_split_v", ImVec2(splitter, topHeight));
	if (ImGui::IsItemHovered() || ImGui::IsItemActive())
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	if (ImGui::IsItemActive())
		m_inspectorWidth = glm::clamp(m_inspectorWidth - ImGui::GetIO().MouseDelta.x, 200.0f, glm::max(avail.x - 220.0f, 200.0f));
	ImGui::SameLine(0.0f, 0.0f);
	if (ImGui::BeginChild("##ae_inspector", ImVec2(inspectorWidth, topHeight), ImGuiChildFlags_Borders))
		renderInspector();
	ImGui::EndChild();

	// ---- The horizontal splitter ----
	ImGui::InvisibleButton("##ae_split_h", ImVec2(avail.x, splitter));
	if (ImGui::IsItemHovered() || ImGui::IsItemActive())
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
	if (ImGui::IsItemActive() && avail.y > 1.0f)
		m_topFraction = glm::clamp(m_topFraction + ImGui::GetIO().MouseDelta.y / avail.y, 0.2f, 0.85f);

	// ---- Bottom: the track list | the timeline ----
	renderTimelineArea();

	commitPendingEdit();
}

void AnimationEditor::renderEmptyState()
{
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + avail.x * 0.5f - 160.0f, ImGui::GetCursorPosY() + avail.y * 0.35f));
	ImGui::BeginGroup();
	ImGui::TextUnformatted("No animation clip open.");
	ImGui::Spacing();
	if (ImGui::Button("New Clip", ImVec2(100, 0)))
		guardedSwitch(PendingSwitch::New);
	ImGui::SameLine();
	if (ImGui::Button("Open...", ImVec2(100, 0)))
		m_openOpenPopup = true;
	ImGui::SameLine();
	if (ImGui::Button("Import...", ImVec2(100, 0)))
	{
		m_importFromAnm = false;
		m_openImportPopup = true;
	}
	ImGui::Spacing();
	ImGui::TextDisabled("Or drop a .clip, a model file or an .anm here.");
	ImGui::EndGroup();
}

void AnimationEditor::renderToolbar()
{
	// File
	if (ImGui::Button("File"))
		ImGui::OpenPopup("##ae_file");
	if (ImGui::BeginPopup("##ae_file"))
	{
		if (ImGui::MenuItem("New clip"))
			guardedSwitch(PendingSwitch::New);
		if (ImGui::MenuItem("Open...", "Ctrl+O"))
			m_openOpenPopup = true;
		if (ImGui::MenuItem("Save", "Ctrl+S", false, m_hasDocument))
		{
			if (m_path.empty())
				m_openSaveAsPopup = true;
			else
				trySave(m_path);
		}
		if (ImGui::MenuItem("Save as...", nullptr, false, m_hasDocument))
			m_openSaveAsPopup = true;
		if (ImGui::MenuItem("Revert", nullptr, false, m_hasDocument && !m_path.empty() && isDirty()))
			doOpen(m_path);
		ImGui::Separator();
		if (ImGui::MenuItem("Import from a model file..."))
		{
			m_importFromAnm = false;
			m_openImportPopup = true;
		}
		if (ImGui::MenuItem("Convert .anm clips..."))
		{
			m_importFromAnm = true;
			m_openImportPopup = true;
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Show in Content", nullptr, false, !m_path.empty()))
			m_revealRequest = m_path;
		if (ImGui::MenuItem("Close", nullptr, false, m_hasDocument))
			guardedSwitch(PendingSwitch::Close);
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	if (m_hasDocument)
		ImGui::Text("%s%s", documentName().c_str(), isDirty() ? " *" : "");
	else
		ImGui::TextDisabled("(no clip)");

	// Rig
	ImGui::SameLine(0.0f, 16.0f);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Rig");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(150.0f);
	if (ImGui::BeginCombo("##ae_rig", m_rigName.empty() ? "(none)" : m_rigName.c_str(), ImGuiComboFlags_HeightLarge))
	{
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		ImGui::InputTextWithHint("##ae_rig_search", "Search", m_rigSearch, sizeof(m_rigSearch));
		oc::vector<oc::string> names;
		for (const auto& kv : Globals::assetRegistry.getObjectContainers())
			if (aeContains(kv.first, m_rigSearch))
				names.push_back(kv.first);
		oc::sort(names.begin(), names.end());
		for (const oc::string& name : names)
		{
			const ObjectContainer* loaded = Globals::world.findLoadedContainer(name);
			const bool skinned = loaded && loaded->isSkinned();
			if (loaded && !skinned)
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			if (ImGui::Selectable(name.c_str(), name == m_rigName))
				setRig(name);
			if (loaded && !skinned)
			{
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Loaded, but it has no skeleton");
			}
		}
		ImGui::EndCombo();
	}
	if (!m_rigName.empty() && m_rigContainer && !m_skeleton)
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "no skeleton");
	}
	else if (!m_rigName.empty() && !m_rigContainer)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("loading...");
	}

	if (!m_hasDocument)
		return;

	// Transport
	ImGui::SameLine(0.0f, 16.0f);
	if (ImGui::Button("|<"))
		setTime(0.0f);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Start (Home)");
	ImGui::SameLine(0.0f, 2.0f);
	if (ImGui::Button("<"))
		stepFrames(-1);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous key step (,)  Alt+, = previous key");
	ImGui::SameLine(0.0f, 2.0f);
	if (ImGui::Button(m_playing ? "Pause" : "Play", ImVec2(48.0f, 0.0f)))
		m_playing = !m_playing;
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Space");
	ImGui::SameLine(0.0f, 2.0f);
	if (ImGui::Button(">"))
		stepFrames(1);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next key step (.)  Alt+. = next key. Past the end, the clip grows by a step.");
	ImGui::SameLine(0.0f, 2.0f);
	if (ImGui::Button(">|"))
		setTime(m_clip.duration);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("End (End)");

	// Record
	ImGui::SameLine(0.0f, 10.0f);
	const bool recording = m_recording;
	if (recording)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.12f, 0.12f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
	}
	if (ImGui::Button("Rec"))
		m_recording = !m_recording;
	if (recording)
		ImGui::PopStyleColor(2);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Record (R): a pose edit keys the bone at the playhead.\nOff: an edit is a preview until the playhead moves; K keys it.");
	ImGui::SameLine();
	if (ImGui::Button("Key"))
		keySelectedBones();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Key the selected bone's pose at the playhead (K)");

	// Frame field
	ImGui::SameLine(0.0f, 10.0f);
	int step = stepIndex(m_time);
	ImGui::SetNextItemWidth(56.0f);
	if (ImGui::DragInt("##ae_step", &step, 0.2f, 0, 100000))
	{
		m_playing = false;
		setTime(float(step) * gridStep());
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The playhead's key step. Past the last step, the clip grows to there.");
	ImGui::SameLine(0.0f, 4.0f);
	ImGui::Text("/ %u   %.3f / %.3f s", m_clip.steps, m_time, m_clip.duration);

	// The key grid: subdivide where the motion needs more keys.
	ImGui::SameLine(0.0f, 10.0f);
	if (ImGui::Button("Subdivide"))
		setSteps(m_clip.steps * 2);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Twice as many key steps over the same length (%u -> %u). The keys keep their times.", m_clip.steps, m_clip.steps * 2);
	ImGui::SameLine(0.0f, 2.0f);
	ImGui::BeginDisabled(m_clip.steps < 2);
	if (ImGui::Button("Merge"))
		setSteps(glm::max(m_clip.steps / 2, 1u));
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Half as many key steps. Keys between the new steps stay where they are.");

	// View + clip menu
	ImGui::SameLine(0.0f, 16.0f);
	if (ImGui::RadioButton("Dopesheet", !m_curveView))
		m_curveView = false;
	ImGui::SameLine();
	if (ImGui::RadioButton("Curves", m_curveView))
	{
		m_curveView = true;
		m_curveFitPending = true;
	}
	ImGui::SameLine(0.0f, 16.0f);
	if (ImGui::Button("Clip"))
		ImGui::OpenPopup("##ae_clip");
	if (ImGui::BeginPopup("##ae_clip"))
	{
		ImGui::Checkbox("Snap to key steps", &m_snap);
		ImGui::Checkbox("Times in seconds", &m_showSeconds);
		ImGui::SetNextItemWidth(80.0f);
		ImGui::DragFloat("Playback speed", &m_playSpeed, 0.01f, -4.0f, 4.0f, "%.2fx");
		if (ImGui::BeginMenu("New keys blend"))
		{
			for (KeyInterp interp : { KeyInterp::Smooth, KeyInterp::Linear, KeyInterp::Step })
				if (ImGui::MenuItem(aeInterpName(interp), nullptr, m_newKeyInterp == interp))
					m_newKeyInterp = interp;
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Reduce keys..."))
			m_openReducePopup = true;
		if (ImGui::MenuItem("Match the end pose to the start", nullptr, false, m_clip.duration > 0.0f))
			matchLoopPose();
		if (ImGui::MenuItem("Remove tracks the rig has no bone for", nullptr, false, m_skeleton != nullptr))
			removeUnboundTracks();
		if (ImGui::MenuItem("Length = last key"))
		{
			float last = 0.0f;
			for (const ClipTrack& t : m_clip.tracks)
				for (uint32 c = 0; c < c_numClipChannels; ++c)
					aeWithKeys(t, ClipChannel(c), [&](const auto& keys) { if (!keys.empty()) last = glm::max(last, keys.back().time); });
			if (last > 0.0f)
				setClipDuration(last, false);
		}
		ImGui::EndPopup();
	}

	// Status line on the right
	if (m_messageTimer > 0.0f && !m_message.empty())
	{
		ImGui::SameLine(0.0f, 16.0f);
		ImGui::TextColored(m_messageIsError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "%s", m_message.c_str());
	}
}

bool AnimationEditor::rotationField(const char* label, glm::quat& quat)
{
	// The typed angles stay as long as they still describe this quaternion.
	if (m_eulerQuat != quat)
	{
		m_eulerQuat = quat;
		m_eulerDegrees = glm::degrees(glm::eulerAngles(quat));
	}
	ImGui::PushID(label);
	aeLabel(label);
	const bool changed = ImGui::DragFloat3("##v", &m_eulerDegrees.x, 0.5f, 0.0f, 0.0f, "%.2f");
	ImGui::PopID();
	if (changed)
	{
		quat = glm::quat(glm::radians(m_eulerDegrees));
		m_eulerQuat = quat;
	}
	return changed;
}

void AnimationEditor::renderInspector()
{
	if (m_skeleton && m_selectedBone >= 0 && (uint32)m_selectedBone < m_skeleton->numBones())
		renderBoneInspector(m_selectedBone);
	else if (m_selectedTrack >= 0 && m_selectedTrack < (int32)m_clip.tracks.size())
	{
		ImGui::SeparatorText("Track");
		ImGui::TextWrapped("%s", m_clip.tracks[m_selectedTrack].bone.c_str());
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.35f, 1.0f), m_skeleton ? "The rig has no bone of this name." : "No rig: pick one to see and pose it.");
	}
	else
	{
		ImGui::SeparatorText("Bone");
		ImGui::TextDisabled(m_skeleton ? "Click a joint in the preview or a row below." : "Pick a rig (toolbar) to pose bones.");
	}
	if (!m_keySelection.empty())
		renderKeyInspector();
	if (m_selectedEvent >= 0 && m_selectedEvent < (int32)m_clip.events.size())
		renderEventInspector();
	renderClipSettings();
}

void AnimationEditor::renderBoneInspector(int32 bone)
{
	ImGui::SeparatorText("Bone");
	const oc::string& name = m_skeleton->boneNames[bone];
	ImGui::TextWrapped("%s", name.c_str());
	const int32 track = (uint32)bone < m_boneTrack.size() ? m_boneTrack[bone] : -1;
	const bool overridden = m_overrides.count(bone) != 0;
	if (overridden)
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Unkeyed edit: K keys it, moving the playhead drops it");
	else if (m_recording)
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Recording: an edit keys step %d", stepIndex(m_time));

	glm::vec3 pos, scale;
	glm::quat rot;
	currentLocalPose(bone, pos, rot, scale);

	// Each field shows whether its channel has a key at the playhead.
	auto keyedHere = [&](ClipChannel channel)
	{
		if (track < 0)
			return false;
		bool hit = false;
		const float eps = gridStep() * 0.25f;
		aeWithKeys(m_clip.tracks[track], channel, [&](const auto& keys) { for (const auto& k : keys) hit |= glm::abs(k.time - m_time) <= eps; });
		return hit;
	};
	auto field = [&](const char* label, ClipChannel channel, auto&& draw)
	{
		const bool keyed = keyedHere(channel);
		const bool animated = track >= 0 && m_clip.tracks[track].numKeys(channel) > 0;
		if (keyed)
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.18f, 0.18f, 0.8f));
		else if (animated)
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.42f, 0.32f, 0.12f, 0.7f));
		draw(label);
		if (keyed || animated)
			ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(keyed ? "Keyed at this frame" : animated ? "Animated (no key at this frame)" : "Not animated");
		(void)channel;
	};

	field("Position", ClipChannel::Position, [&](const char* label)
	{
		ImGui::PushID(label);
		aeLabel(label);
		glm::vec3 p = pos;
		if (ImGui::DragFloat3("##v", &p.x, 0.001f, 0.0f, 0.0f, "%.4f"))
			editBonePose(bone, ClipChannel::Position, p, rot, scale);
		ImGui::PopID();
	});
	field("Rotation", ClipChannel::Rotation, [&](const char* label)
	{
		glm::quat r = rot;
		if (rotationField(label, r))
			editBonePose(bone, ClipChannel::Rotation, pos, r, scale);
	});
	field("Scale", ClipChannel::Scale, [&](const char* label)
	{
		ImGui::PushID(label);
		aeLabel(label);
		glm::vec3 s = scale;
		if (ImGui::DragFloat3("##v", &s.x, 0.001f, 0.0f, 0.0f, "%.4f"))
			editBonePose(bone, ClipChannel::Scale, pos, rot, s);
		ImGui::PopID();
	});

	if (ImGui::Button("Key all"))
		keyBone(bone, true, true, true);
	ImGui::SameLine();
	if (ImGui::Button("Pos"))
		keyBone(bone, true, false, false);
	ImGui::SameLine();
	if (ImGui::Button("Rot"))
		keyBone(bone, false, true, false);
	ImGui::SameLine();
	if (ImGui::Button("Scale"))
		keyBone(bone, false, false, true);
	if (ImGui::Button("Reset to bind pose"))
	{
		glm::vec3 bp, bs;
		glm::quat br;
		const glm::mat4& bind = m_skeleton->localBind[bone];
		bp = glm::vec3(bind[3]);
		bs = glm::vec3(glm::length(glm::vec3(bind[0])), glm::length(glm::vec3(bind[1])), glm::length(glm::vec3(bind[2])));
		br = glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(bind[0]) / bs.x, glm::vec3(bind[1]) / bs.y, glm::vec3(bind[2]) / bs.z)));
		editBonePose(bone, ClipChannel::Rotation, bp, br, bs);
		editBonePose(bone, ClipChannel::Position, bp, br, bs);
	}
	if (track >= 0)
	{
		ImGui::SameLine();
		if (ImGui::Button("Delete track"))
			deleteTrack(track);
	}
	const int32 parent = m_skeleton->parentIndices[bone];
	if (parent >= 0)
	{
		ImGui::TextDisabled("Parent:");
		ImGui::SameLine();
		if (ImGui::SmallButton(m_skeleton->boneNames[parent].c_str()))
			m_selectedBone = parent;
	}
}

void AnimationEditor::renderKeyInspector()
{
	ImGui::SeparatorText("Keys");
	oc::vector<KeyRef> keys;
	collectSelectedKeys(keys);
	ImGui::Text("%u selected", (uint32)keys.size());

	// Their blend mode, "mixed" when they differ.
	int mode = -1;
	for (const KeyRef& ref : keys)
	{
		if (ref.track >= m_clip.tracks.size())
			continue;
		aeWithKeys(m_clip.tracks[ref.track], ref.channel, [&](const auto& ks)
		{
			for (const auto& k : ks)
				if (k.time == ref.time)
					mode = mode == -1 ? (int)k.interp : (mode == (int)k.interp ? mode : -2);
		});
	}
	aeLabel("Blend");
	const char* preview = mode < 0 ? (mode == -2 ? "(mixed)" : "") : aeInterpName(KeyInterp(mode));
	if (ImGui::BeginCombo("##ae_interp", preview))
	{
		for (KeyInterp interp : { KeyInterp::Linear, KeyInterp::Smooth, KeyInterp::Step })
			if (ImGui::Selectable(aeInterpName(interp), mode == (int)interp))
				setSelectedKeysInterp(interp);
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("How a key blends to the next key of its channel");

	// The earliest key's frame: moving it moves them all.
	float earliest = FLT_MAX;
	for (const KeyRef& ref : keys)
		earliest = glm::min(earliest, ref.time);
	aeLabel(keys.size() == 1 ? "Step" : "First step");
	int step = stepIndex(earliest);
	if (ImGui::InputInt("##ae_keystep", &step, 1, 5) && step >= 0)
		moveSelectedKeys(float(step) * gridStep() - earliest);
	aeLabel("Time");
	float seconds = earliest;
	if (ImGui::DragFloat("##ae_keytime", &seconds, 0.001f, 0.0f, 600.0f, "%.4f s") && seconds >= 0.0f)
		moveSelectedKeys(seconds - earliest); // off the grid on purpose: a typed time is exact

	if (keys.size() == 1 && keys[0].track < m_clip.tracks.size())
	{
		const KeyRef ref = keys[0];
		ClipTrack& track = m_clip.tracks[ref.track];
		ImGui::TextDisabled("%s / %s", track.bone.c_str(), ref.channel == ClipChannel::Position ? "Position" : ref.channel == ClipChannel::Rotation ? "Rotation" : "Scale");
		if (ref.channel == ClipChannel::Rotation)
		{
			for (RotationKey& k : track.rotationKeys)
				if (k.time == ref.time)
				{
					glm::quat q = k.value;
					if (rotationField("Value", q))
					{
						k.value = glm::normalize(q);
						markEdited();
					}
				}
		}
		else
		{
			aeWithKeys(track, ref.channel, [&](auto& ks)
			{
				using KeyT = typename std::remove_reference_t<decltype(ks)>::value_type;
				if constexpr (!std::is_same_v<KeyT, RotationKey>)
					for (KeyT& k : ks)
						if (k.time == ref.time)
						{
							aeLabel("Value");
							if (ImGui::DragFloat3("##ae_keyvalue", &k.value.x, 0.001f, 0.0f, 0.0f, "%.4f"))
								markEdited();
						}
			});
		}
	}
	if (ImGui::Button("Delete keys"))
		deleteSelectedKeys();
	ImGui::SameLine();
	if (ImGui::Button("Copy"))
		copySelectedKeys();
}

void AnimationEditor::renderEventInspector()
{
	ImGui::SeparatorText("Event");
	AnimationEventKey& ev = m_clip.events[m_selectedEvent];
	if (m_eventNameFor != m_selectedEvent || (!ImGui::IsAnyItemActive() && ev.name != m_eventNameBuf))
	{
		aeCopyString(m_eventNameBuf, sizeof(m_eventNameBuf), ev.name);
		m_eventNameFor = m_selectedEvent;
	}
	aeLabel("Name");
	if (ImGui::InputText("##ae_evname", m_eventNameBuf, sizeof(m_eventNameBuf)))
	{
		ev.name = m_eventNameBuf;
		markEdited();
	}
	const float duration = glm::max(m_clip.duration, 1e-6f);
	int step = stepIndex(ev.normalizedTime * duration);
	aeLabel("Step");
	if (ImGui::InputInt("##ae_evstep", &step, 1, 5))
	{
		ev.normalizedTime = glm::clamp(float(step) * gridStep() / duration, 0.0f, 1.0f);
		markEdited();
	}
	ImGui::TextDisabled("Fires into the entity's script / animator onEvent");
	if (ImGui::Button("Delete event"))
		deleteEvent(m_selectedEvent);
}

void AnimationEditor::renderClipSettings()
{
	ImGui::Spacing();
	if (!ImGui::CollapsingHeader("Clip", ImGuiTreeNodeFlags_DefaultOpen))
		return;
	// Typed, not dragged: a whole-step trim rounds every small drag back to where it was.
	aeLabel("Length");
	float seconds = m_clip.duration;
	if (ImGui::InputFloat("##ae_len", &seconds, 0.0f, 0.0f, "%.3f s", ImGuiInputTextFlags_EnterReturnsTrue) && seconds > 0.0f)
		setClipDuration(seconds, m_lengthScalesKeys);
	if (ImGui::BeginPopupContextItem("##ae_len_ctx"))
	{
		if (ImGui::MenuItem("Half speed (keys x2)"))
			setClipDuration(m_clip.duration * 2.0f, true);
		if (ImGui::MenuItem("Double speed (keys x0.5)"))
			setClipDuration(m_clip.duration * 0.5f, true);
		ImGui::EndPopup();
	}
	ImGui::Dummy(ImVec2(92.0f - ImGui::GetStyle().ItemSpacing.x, 0.0f));
	ImGui::SameLine();
	ImGui::Checkbox("Keys scale with the length", &m_lengthScalesKeys);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("On: the keys stay on their steps and the step gets longer or shorter.\n"
			"Off: the step stays; the clip is trimmed or extended by whole steps.");
	aeLabel("Key steps");
	int steps = (int)m_clip.steps;
	if (ImGui::InputInt("##ae_steps", &steps, 1, 10) && steps >= 1)
		setSteps((uint32)steps);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The length in this many equal steps; keys snap to them. A step is %.4f s.\n"
			"The keys keep their times when this changes.", gridStep());
	aeLabel("Loop");
	if (ImGui::Checkbox("##ae_loop", &m_clip.loop))
		markEdited();
	ImGui::TextDisabled("%u tracks, %u keys, %u events", (uint32)m_clip.tracks.size(), m_clip.numKeys(), (uint32)m_clip.events.size());
	if (!m_path.empty())
		ImGui::TextDisabled("%s", m_path.c_str());
	ImGui::TextDisabled("Plays as clip '%s' (an .apl's Clip / Play)", documentName().c_str());
}

// ---- Popups ----

void AnimationEditor::renderPopups()
{
	if (m_openUnsavedPopup)   { ImGui::OpenPopup("Unsaved animation##ae"); m_openUnsavedPopup = false; }
	if (m_openSaveAsPopup)
	{
		if (m_saveAsName[0] == '\0' || !m_path.empty())
			aeCopyString(m_saveAsName, sizeof(m_saveAsName), m_path.empty() ? oc::string("NewClip") : FileSystem::stem(m_path));
		if (!m_path.empty())
			aeCopyString(m_saveAsFolder, sizeof(m_saveAsFolder), FileSystem::parentPath(m_path));
		ImGui::OpenPopup("Save clip as##ae");
		m_openSaveAsPopup = false;
	}
	if (m_openOverwritePopup) { ImGui::OpenPopup("Overwrite clip##ae"); m_openOverwritePopup = false; }
	if (m_openOpenPopup)      { ImGui::OpenPopup("Open clip##ae"); m_openOpenPopup = false; m_clipFiles.clear(); }
	if (m_openReducePopup)    { ImGui::OpenPopup("Reduce keys##ae"); m_openReducePopup = false; }
	if (m_openImportPopup)
	{
		ImGui::OpenPopup("Import animation##ae");
		m_openImportPopup = false;
		m_modelFiles.clear();
		oc::vector<FileSystem::DirEntry> entries;
		if (FileSystem::listDirectoryRecursive(FileSystem::rootPath(), entries, /*allowMainThread*/ true))
			for (const FileSystem::DirEntry& e : entries)
				if (!e.isDirectory && aeIsModelFile(e.path))
				{
					const oc::string rel = FileSystem::lexicallyRelative(e.path, FileSystem::rootPath());
					if (rel.rfind("Local/", 0) != 0)
						m_modelFiles.push_back(rel);
				}
		oc::sort(m_modelFiles.begin(), m_modelFiles.end());
	}

	renderUnsavedPopup();
	renderSaveAsPopup();
	renderOpenPopup();
	renderImportPopup();
	renderReducePopup();

	if (ImGui::BeginPopupModal("Overwrite clip##ae", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("'%s' exists. Overwrite it?", m_pendingSavePath.c_str());
		if (ImGui::Button("Overwrite"))
		{
			queueSave(m_pendingSavePath);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void AnimationEditor::renderUnsavedPopup()
{
	if (!ImGui::BeginPopupModal("Unsaved animation##ae", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	ImGui::Text("'%s' has unsaved changes.", documentName().c_str());
	if (ImGui::Button("Save"))
	{
		if (m_path.empty())
			m_openSaveAsPopup = true; // the switch is dropped: save first, then repeat it
		else
		{
			queueSave(m_path);
			runSwitch(m_pendingSwitch);
		}
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Discard"))
	{
		m_editPending = false;
		m_dirty = false;
		runSwitch(m_pendingSwitch);
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
	{
		m_pendingSwitch = PendingSwitch::None;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void AnimationEditor::renderSaveAsPopup()
{
	if (!ImGui::BeginPopupModal("Save clip as##ae", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	ImGui::TextUnformatted("The file name is the clip's name: an .apl plays it by that name.");
	ImGui::SetNextItemWidth(260.0f);
	if (ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	const bool enter = ImGui::InputText("Name", m_saveAsName, sizeof(m_saveAsName), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SetNextItemWidth(260.0f);
	ImGui::InputText("Folder", m_saveAsFolder, sizeof(m_saveAsFolder));
	const oc::string name = m_saveAsName;
	if (!name.empty() && Globals::assetRegistry.getAnimationClips().count(name) != 0)
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Replaces the .anm clip '%s' at runtime", name.c_str());
	if ((ImGui::Button("Save") || enter) && !name.empty())
	{
		trySave(FileSystem::join(m_saveAsFolder, name + ".clip"));
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void AnimationEditor::renderOpenPopup()
{
	ImGui::SetNextWindowSize(ImVec2(420.0f, 420.0f), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("Open clip##ae", nullptr))
		return;
	if (m_clipFiles.empty())
	{
		for (const auto& kv : Globals::assetRegistry.getClipFiles())
			m_clipFiles.push_back(kv.second);
		oc::sort(m_clipFiles.begin(), m_clipFiles.end());
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##ae_open_search", "Search clips", m_openSearch, sizeof(m_openSearch));
	if (ImGui::BeginChild("##ae_open_list", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders))
	{
		for (const oc::string& path : m_clipFiles)
		{
			if (!aeContains(path, m_openSearch))
				continue;
			if (ImGui::Selectable(path.c_str(), FileSystem::pathEquals(path, m_path), ImGuiSelectableFlags_AllowDoubleClick)
				&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				requestOpen(path);
				ImGui::CloseCurrentPopup();
			}
		}
		if (m_clipFiles.empty())
			ImGui::TextDisabled("No .clip files yet: import one, or start a new clip.");
	}
	ImGui::EndChild();
	if (ImGui::Button("Cancel"))
		ImGui::CloseCurrentPopup();
	ImGui::SameLine();
	ImGui::TextDisabled("Double-click to open");
	ImGui::EndPopup();
}

void AnimationEditor::renderReducePopup()
{
	if (!ImGui::BeginPopupModal("Reduce keys##ae", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	ImGui::TextUnformatted("Drops every Linear key the line between its neighbours reproduces.");
	ImGui::SetNextItemWidth(120.0f);
	ImGui::DragFloat("Position error", &m_tolerance.position, 0.0001f, 0.0f, 1.0f, "%.4f");
	ImGui::SetNextItemWidth(120.0f);
	ImGui::DragFloat("Rotation error", &m_tolerance.rotationDegrees, 0.01f, 0.0f, 10.0f, "%.2f deg");
	ImGui::SetNextItemWidth(120.0f);
	ImGui::DragFloat("Scale error", &m_tolerance.scale, 0.0001f, 0.0f, 1.0f, "%.4f");
	if (ImGui::Button("Reduce"))
	{
		reduceKeys();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void AnimationEditor::renderImportPopup()
{
	ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("Import animation##ae", nullptr))
		return;

	if (ImGui::RadioButton("From a model file", !m_importFromAnm))
		m_importFromAnm = false;
	ImGui::SameLine();
	if (ImGui::RadioButton("Convert .anm clips", m_importFromAnm))
		m_importFromAnm = true;
	ImGui::TextDisabled(m_importFromAnm
		? "Each picked .anm clip becomes a .clip of the same name, with its loop flag and events. The .clip then plays in its place."
		: "Every animation track of the file becomes a .clip (a file with several gets _0, _1, ...).");

	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##ae_imp_search", "Search", m_importSearch, sizeof(m_importSearch));
	const float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 6.5f;
	if (ImGui::BeginChild("##ae_imp_list", ImVec2(0.0f, glm::max(listHeight, 80.0f)), ImGuiChildFlags_Borders))
	{
		if (m_importFromAnm)
		{
			oc::vector<oc::string> names;
			for (const auto& kv : Globals::assetRegistry.getAnimationClips())
				if (aeContains(kv.first, m_importSearch))
					names.push_back(kv.first);
			oc::sort(names.begin(), names.end());
			for (const oc::string& name : names)
			{
				bool picked = m_importAnmPicked.count(name) != 0;
				if (ImGui::Checkbox(name.c_str(), &picked))
				{
					if (picked) m_importAnmPicked.insert(name);
					else m_importAnmPicked.erase(name);
				}
				const AnimationClipDesc& desc = Globals::assetRegistry.getAnimationClips().find(name)->second;
				ImGui::SameLine();
				ImGui::TextDisabled("%s%s", desc.source.c_str(), Globals::assetRegistry.findClipFile(name) ? "   (has a .clip)" : "");
			}
			if (names.empty())
				ImGui::TextDisabled("No .anm clips are registered.");
		}
		else
		{
			for (const oc::string& path : m_modelFiles)
				if (aeContains(path, m_importSearch) && ImGui::Selectable(path.c_str(), path == m_importSource))
					m_importSource = path;
			if (m_modelFiles.empty())
				ImGui::TextDisabled("No model files under Assets/.");
		}
	}
	ImGui::EndChild();

	if (m_importFromAnm)
	{
		if (ImGui::SmallButton("All"))
			for (const auto& kv : Globals::assetRegistry.getAnimationClips())
				m_importAnmPicked.insert(kv.first);
		ImGui::SameLine();
		if (ImGui::SmallButton("None"))
			m_importAnmPicked.clear();
	}
	else
	{
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputTextWithHint("Skip tracks named", "e.g. Pose", m_importSkip, sizeof(m_importSkip));
	}
	ImGui::SetNextItemWidth(260.0f);
	ImGui::InputText("Into folder", m_importFolder, sizeof(m_importFolder));
	ImGui::Checkbox("Reduce keys", &m_importReduce);
	if (m_importReduce)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f);
		ImGui::DragFloat("##ae_imp_rot", &m_tolerance.rotationDegrees, 0.01f, 0.0f, 10.0f, "%.2f deg");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f);
		ImGui::DragFloat("##ae_imp_pos", &m_tolerance.position, 0.0001f, 0.0f, 1.0f, "%.4f m");
		m_tolerance.scale = m_tolerance.position;
	}
	ImGui::TextDisabled("Preview rig: %s", m_rigName.empty() ? "(none: pick one in the toolbar)" : m_rigName.c_str());

	const bool canImport = m_importFromAnm ? !m_importAnmPicked.empty() : !m_importSource.empty();
	ImGui::BeginDisabled(!canImport);
	if (ImGui::Button("Import"))
	{
		auto baseJob = [&]()
		{
			ImportJob job;
			job.folder = m_importFolder;
			job.rig = m_rigName;
			job.reduce = m_importReduce;
			job.tolerance = m_tolerance;
			return job;
		};
		if (m_importFromAnm)
		{
			oc::vector<oc::string> names(m_importAnmPicked.begin(), m_importAnmPicked.end());
			oc::sort(names.begin(), names.end());
			for (const oc::string& name : names)
			{
				const auto it = Globals::assetRegistry.getAnimationClips().find(name);
				if (it == Globals::assetRegistry.getAnimationClips().end())
					continue;
				const AnimationClipDesc& desc = it->second;
				ImportJob job = baseJob();
				job.source = desc.source;
				if (const ObjectContainerDesc* container = Globals::assetRegistry.findObjectContainer(desc.source))
					job.source = container->path;
				job.name = desc.name;
				job.skip = desc.skip;
				job.track = desc.track;
				job.fromAnm = true;
				job.loop = desc.loop;
				for (const auto& [eventName, t] : desc.events)
					job.events.push_back({ eventName, t });
				job.openAfter = &name == &names.front();
				m_importJobs.push_back(oc::move(job));
			}
		}
		else
		{
			ImportJob job = baseJob();
			job.source = m_importSource;
			job.name = FileSystem::stem(m_importSource);
			job.skip = m_importSkip;
			job.openAfter = true;
			m_importJobs.push_back(oc::move(job));
		}
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

export module UI:GameHudOverlay;

import Core;
import Core.imgui;
import Core.glm;
import Core.Rect;
import Core.GameHud;
import Core.Tweaks;

// Draws the in-game HUD (Core.GameHud is the model; scripts write it) OVER the viewport: a
// Minecraft-style hotbar bottom-center (keys 1..9,0 select -- see InputControls) and bars/counters
// stacked from the top left. Pure ImDrawList painting into the Viewport window's draw list -- no
// ImGui widgets, so it never takes focus or eats input. UI::update calls render() while the
// Viewport window is current.
export class GameHudOverlay final
{
public:

	void registerTweaks()
	{
		Tweak::boolean("HUD", "Enabled", &m_enabled);
		Tweak::floatVar("HUD", "Scale", &m_scale, 0.5f, 3.0f, 0.05f);
		Tweak::floatVar("HUD", "Opacity", &m_opacity, 0.1f, 1.0f, 0.05f);
		Tweak::floatVar("HUD", "Hotbar slot size", &m_hotbarSlotSize, 24.0f, 160.0f, 1.0f);
		Tweak::floatVar("HUD", "Hotbar text scale", &m_hotbarTextScale, 0.5f, 4.0f, 0.05f);
	}

	void render(const Rect& viewport)
	{
		if (!m_enabled)
			return;
		const glm::ivec2 size = viewport.getSize();
		if (size.x <= 0 || size.y <= 0)
			return;
		const GameHud::Snapshot hud = Globals::gameHud.snapshot();
		if (!hud.hotbarActive)
			Globals::gameHud.setSlotScreenRects({}); // nothing drawn = nothing clickable
		if (!hud.hotbarActive && hud.bars.empty() && hud.counters.empty() && hud.worldLabels.empty())
			return;

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 vpMin((float)viewport.min.x, (float)viewport.min.y);
		const ImVec2 vpMax((float)viewport.max.x, (float)viewport.max.y);
		dl->PushClipRect(vpMin, vpMax, true);

		ImFont* font = ImGui::GetFont();
		const float s = m_scale;
		const float alpha = m_opacity;
		const auto col = [alpha](float r, float g, float b, float a)
		{
			return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a * alpha));
		};
		const auto colV = [alpha](const glm::vec3& c, float a)
		{
			return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a * alpha));
		};
		// Every string draws twice, shadow first -- the HUD sits over arbitrary scene content.
		const auto text = [&](ImVec2 pos, float fontSize, ImU32 color, const char* str)
		{
			dl->AddText(font, fontSize, ImVec2(pos.x + 1.0f, pos.y + 1.0f), col(0.0f, 0.0f, 0.0f, 0.7f), str);
			dl->AddText(font, fontSize, pos, color, str);
		};
		const float fontSize = ImGui::GetFontSize() * s;
		constexpr float noWrap = 1.0e30f; // CalcTextSizeA max width: never wrap
		char buf[64];

		// ---- top left: bars, then counters, each entry under the previous ----
		{
			const float margin = 14.0f * s;
			const float rowW = 230.0f * s;
			const float barH = 20.0f * s;
			const float gap = 6.0f * s;
			const float x = vpMin.x + margin;
			float y = vpMin.y + margin;
			for (const HudBar& bar : hud.bars)
			{
				const ImVec2 bMin(x, y), bMax(x + rowW, y + barH);
				dl->AddRectFilled(bMin, bMax, col(0.06f, 0.06f, 0.08f, 0.75f), 4.0f * s);
				const float frac = glm::clamp(bar.value / bar.maxValue, 0.0f, 1.0f); // maxValue > 0 guaranteed by setBar
				if (frac > 0.0f)
					dl->AddRectFilled(bMin, ImVec2(x + rowW * frac, y + barH), colV(bar.color, 0.9f), 4.0f * s);
				dl->AddRect(bMin, bMax, col(0.0f, 0.0f, 0.0f, 0.9f), 4.0f * s, 0, 1.5f * s);
				const float textY = y + (barH - fontSize) * 0.5f;
				text(ImVec2(x + 6.0f * s, textY), fontSize, col(1.0f, 1.0f, 1.0f, 1.0f), bar.name.c_str());
				formatBarValue(buf, sizeof(buf), bar.value, bar.maxValue);
				const ImVec2 valSize = font->CalcTextSizeA(fontSize, noWrap, 0.0f, buf);
				text(ImVec2(x + rowW - valSize.x - 6.0f * s, textY), fontSize, col(1.0f, 1.0f, 1.0f, 1.0f), buf);
				y += barH + gap;
			}
			for (const HudCounter& counter : hud.counters)
			{
				snprintf(buf, sizeof(buf), "%.*f", counter.decimals, counter.value);
				text(ImVec2(x, y), fontSize, col(0.85f, 0.85f, 0.85f, 1.0f), counter.name.c_str());
				const ImVec2 valSize = font->CalcTextSizeA(fontSize, noWrap, 0.0f, buf);
				text(ImVec2(x + rowW - valSize.x, y), fontSize, colV(counter.color, 1.0f), buf);
				y += fontSize + gap * 0.7f;
			}
		}

		// ---- world-anchored labels (screenPos pre-projected by gameplay): title above the bar,
		// ---- the bar centered on the anchor, info lines below. Drawn under the hotbar. ----
		for (const HudWorldLabel& label : hud.worldLabels)
		{
			const float e = label.emphasized ? 1.3f : 1.0f;
			if (label.screenPos.x < vpMin.x - 100.0f || label.screenPos.x > vpMax.x + 100.0f
				|| label.screenPos.y < vpMin.y - 100.0f || label.screenPos.y > vpMax.y + 100.0f)
				continue; // fully off the viewport (the clip rect would eat it anyway)
			const float labelFont = fontSize * 0.78f * e;
			const float titleFont = fontSize * 0.95f * e; // the type tag reads at a glance
			float y = label.screenPos.y;
			if (!label.title.empty())
			{
				const ImVec2 ts = font->CalcTextSizeA(titleFont, noWrap, 0.0f, label.title.c_str());
				text(ImVec2(label.screenPos.x - ts.x * 0.5f, y - ts.y - 2.0f * s), titleFont,
					col(1.0f, 1.0f, 1.0f, 1.0f), label.title.c_str());
			}
			const auto miniBar = [&](float value, float maxValue, const glm::vec3& color)
			{
				const float barW = 52.0f * s * e;
				const float barH = 6.0f * s * e;
				const ImVec2 bMin(label.screenPos.x - barW * 0.5f, y);
				const ImVec2 bMax(label.screenPos.x + barW * 0.5f, y + barH);
				dl->AddRectFilled(bMin, bMax, col(0.06f, 0.06f, 0.08f, 0.75f), 2.0f * s);
				const float frac = glm::clamp(value / maxValue, 0.0f, 1.0f);
				if (frac > 0.0f)
					dl->AddRectFilled(bMin, ImVec2(bMin.x + barW * frac, bMax.y), colV(color, 0.95f), 2.0f * s);
				dl->AddRect(bMin, bMax, col(0.0f, 0.0f, 0.0f, 0.9f), 2.0f * s, 0, 1.0f * s);
				y += barH + 3.0f * s;
			};
			if (label.barMax > 0.0f)
				miniBar(label.barValue, label.barMax, label.barColor);
			if (label.bar2Max > 0.0f)
				miniBar(label.bar2Value, label.bar2Max, label.bar2Color);
			// info: '\n'-separated lines, centered under the bar
			const char* line = label.info.c_str();
			while (*line)
			{
				const char* end = strchr(line, '\n');
				const size_t len = end ? size_t(end - line) : strlen(line);
				char lineBuf[96];
				snprintf(lineBuf, sizeof(lineBuf), "%.*s", (int)len, line);
				const ImVec2 ts = font->CalcTextSizeA(labelFont, noWrap, 0.0f, lineBuf);
				text(ImVec2(label.screenPos.x - ts.x * 0.5f, y), labelFont, col(0.9f, 0.9f, 0.9f, 1.0f), lineBuf);
				y += ts.y + 1.0f * s;
				line += len + (end ? 1 : 0);
			}
		}

		// ---- bottom LEFT: the hotbar (one row, or a columns-wide GRID -- the RTS QWER/ASDF/ZXCV
		// ---- pattern -- laid out row-major, bottom-anchored). Each slot's rect is reported back so
		// ---- clicks can be resolved against it, and the slot under the cursor lights up. ----
		if (hud.hotbarActive)
		{
			const float slot = m_hotbarSlotSize * s;
			const float pad = 5.0f * s;
			const float margin = 14.0f * s;
			const int columns = hud.columns > 0 ? hud.columns : GameHud::NumSlots;
			const int rows = (GameHud::NumSlots + columns - 1) / columns;
			const float totalH = rows * slot + (rows - 1) * pad;
			const float x0 = vpMin.x + margin;
			const float y0 = vpMax.y - totalH - margin;
			const float keyFontSize = fontSize * 0.95f;
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			glm::vec4 rects[GameHud::NumSlots];
			for (int i = 0; i < GameHud::NumSlots; ++i)
			{
				const float x = x0 + (i % columns) * (slot + pad);
				const float y = y0 + (i / columns) * (slot + pad);
				const HudSlot& hudSlot = hud.slots[i];
				const bool selected = i == hud.selectedSlot;
				const ImVec2 sMin(x, y), sMax(x + slot, y + slot);
				rects[i] = glm::vec4(sMin.x, sMin.y, sMax.x, sMax.y);
				// hover: only ASSIGNED slots light up -- an empty cell is not a button
				const bool hovered = hudSlot.used && mouse.x >= sMin.x && mouse.x <= sMax.x
					&& mouse.y >= sMin.y && mouse.y <= sMax.y;
				dl->AddRectFilled(sMin, sMax, selected ? col(0.10f, 0.12f, 0.16f, 0.9f)
					: hovered ? col(0.16f, 0.18f, 0.22f, 0.85f) : col(0.06f, 0.06f, 0.08f, 0.65f), 5.0f * s);
				if (selected)
					dl->AddRect(ImVec2(sMin.x - 2.0f * s, sMin.y - 2.0f * s), ImVec2(sMax.x + 2.0f * s, sMax.y + 2.0f * s),
						col(1.0f, 1.0f, 1.0f, 0.95f), 6.0f * s, 0, 2.5f * s);
				else if (hovered)
					dl->AddRect(sMin, sMax, col(1.0f, 1.0f, 1.0f, 0.8f), 5.0f * s, 0, 2.0f * s);
				else
					dl->AddRect(sMin, sMax, col(0.5f, 0.5f, 0.55f, 0.6f), 5.0f * s, 0, 1.0f * s);
				// the key that selects this slot, top left corner
				text(ImVec2(sMin.x + 3.0f * s, sMin.y + 2.0f * s), keyFontSize, col(0.7f, 0.7f, 0.7f, 0.9f), hud.keyLabels[i].c_str());
				if (!hudSlot.used)
					continue;
				// item label centered, shrunk to fit when long (icons can come later; text always
				// works). Slot captions are short tags (3-5 chars), so they carry the slot: the
				// base size is deliberately large and only over-long labels scale back.
				if (!hudSlot.label.empty())
				{
					float labelSize = fontSize * m_hotbarTextScale;
					ImVec2 ts = font->CalcTextSizeA(labelSize, noWrap, 0.0f, hudSlot.label.c_str());
					const float maxW = slot - 8.0f * s;
					if (ts.x > maxW)
					{
						labelSize *= maxW / ts.x;
						ts = font->CalcTextSizeA(labelSize, noWrap, 0.0f, hudSlot.label.c_str());
					}
					text(ImVec2(x + (slot - ts.x) * 0.5f, y + (slot - ts.y) * 0.5f), labelSize, col(1.0f, 1.0f, 1.0f, 1.0f), hudSlot.label.c_str());
				}
				if (hudSlot.count > 0) // stack count, bottom right
				{
					snprintf(buf, sizeof(buf), "%d", hudSlot.count);
					const ImVec2 cs = font->CalcTextSizeA(keyFontSize, noWrap, 0.0f, buf);
					text(ImVec2(sMax.x - cs.x - 3.0f * s, sMax.y - cs.y - 2.0f * s), keyFontSize, col(1.0f, 0.95f, 0.6f, 1.0f), buf);
				}
			}
			Globals::gameHud.setSlotScreenRects(rects);
		}

		dl->PopClipRect();
	}

private:

	// "72 / 100": integers when the values are whole, one decimal otherwise.
	static void formatBarValue(char* buf, size_t bufSize, float value, float maxValue)
	{
		const bool whole = glm::abs(value - glm::round(value)) < 0.001f && glm::abs(maxValue - glm::round(maxValue)) < 0.001f;
		snprintf(buf, bufSize, whole ? "%.0f / %.0f" : "%.1f / %.1f", value, maxValue);
	}

	bool  m_enabled = true;
	float m_scale = 1.0f;
	float m_opacity = 0.9f;
	float m_hotbarSlotSize = 72.0f; // px at scale 1 (the grid is the main build UI — big enough to read)
	float m_hotbarTextScale = 1.9f; // slot caption size, x the base font
};

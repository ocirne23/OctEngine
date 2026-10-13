module Procedural;

import Core;
import Core.glm;
import RendererVK; // RendererVKLayout::CLUTTER_GRAIN_STRIP_WIDEN

import :ClutterTexture;
import :ClutterType;
import :TreeGenerator; // treeHash

namespace
{
	using namespace Procedural;

	constexpr float PI = 3.14159265f;
	constexpr uint32 FERN_SIZE = 256;
	constexpr uint32 PETAL_SIZE = 128;
	constexpr uint32 GRAIN_SIZE = 512; // the strip is CLUTTER_GRAIN_STRIP_WIDEN x the ear: the ear keeps ~100 texels across

	float rnd(uint32 seed, uint32 salt) { return treeHash01(treeHash(seed, salt)); }

	// One texel: R the shading, G the second-colour mask, A the cover. A bare texel keeps a mid shade (the mips blend
	// toward it, not toward black).
	void writeTexel(oc::vector<uint8>& out, uint32 size, uint32 x, uint32 y, float shade, float mask, float alpha)
	{
		const size_t i = ((size_t)y * size + x) * 4;
		const bool bare = alpha <= 0.0f;
		out[i + 0] = (uint8)(glm::clamp(bare ? 0.85f : shade, 0.0f, 1.0f) * 255.0f + 0.5f);
		out[i + 1] = (uint8)(glm::clamp(bare ? 0.0f : mask, 0.0f, 1.0f) * 255.0f + 0.5f);
		out[i + 2] = 0;
		out[i + 3] = (uint8)(glm::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
	}

	// A FERN FROND, drawn in its PHYSICAL proportions (Stem long, 2 x HeadSize wide), so the pinnae keep their angle: a
	// stipe (the first 15 %), 12..18 pinna pairs (by the seed) leaning toward the tip, the longest at ~40 %, each cut
	// into pinnules (PetalWidth = how much of the pinna's width the notches keep). The rachis toward Color2.
	void fernFrond(const ClutterTypeDesc& type, uint32 size, oc::vector<uint8>& out)
	{
		constexpr float STIPE = 0.15f;
		const float width = glm::clamp(2.0f * type.headSize / glm::max(type.stemHeight, 1e-3f), 0.05f, 2.0f);
		const uint32 pairs = 12u + treeHash(type.seed, 701u) % 7u;
		const float pinnaAngle = glm::radians(55.0f + 15.0f * rnd(type.seed, 702u)); // from the rachis, toward the tip
		const float pinnules = 5.0f + 4.0f * rnd(type.seed, 703u);                  // lobes along a pinna
		const float cut = 1.0f - glm::clamp(type.petalWidth, 0.0f, 1.0f);
		const float spacing = (1.0f - STIPE) / (float)pairs;
		const float pixel = 1.0f / (float)size; // in length units; the antialiasing band

		struct Pinna { glm::vec2 base; glm::vec2 dir; float len; float halfWidth; };
		oc::vector<Pinna> pinnae;
		for (uint32 k = 0; k < pairs; ++k)
		{
			const float s = ((float)k + 0.5f) / (float)pairs;
			const float envelope = std::sin(PI * std::pow(s, 0.75f));
			const float len = 0.48f * width * envelope;
			for (int side = -1; side <= 1; side += 2)
			{
				Pinna p;
				p.base = glm::vec2(0.0f, STIPE + (1.0f - STIPE) * s);
				p.dir = glm::vec2((float)side * std::sin(pinnaAngle), std::cos(pinnaAngle));
				p.len = len / std::sin(pinnaAngle);
				p.halfWidth = 0.42f * spacing * (0.6f + 0.4f * envelope);
				pinnae.push_back(p);
			}
		}

		for (uint32 y = 0; y < size; ++y)
			for (uint32 x = 0; x < size; ++x)
			{
				const glm::vec2 p(((float)x + 0.5f) / (float)size * width - 0.5f * width, ((float)y + 0.5f) / (float)size);
				float alpha = 0.0f, shade = 0.0f, mask = 0.0f;
				const float rachisHalf = 0.012f * width * glm::mix(1.6f, 0.6f, p.y) + 0.004f;
				const float rachis = glm::clamp((rachisHalf - std::abs(p.x)) / pixel + 0.5f, 0.0f, 1.0f) * (p.y < 0.99f ? 1.0f : 0.0f);
				if (rachis > 0.0f)
				{
					alpha = rachis;
					shade = glm::mix(0.6f, 0.8f, p.y);
					mask = 1.0f - p.y; // Color2 at the stipe, the leaf colour up the blade
				}
				for (const Pinna& pin : pinnae)
				{
					const glm::vec2 d = p - pin.base;
					const float t = glm::dot(d, pin.dir) / pin.len;
					if (t < 0.0f || t > 1.0f)
						continue;
					const float across = std::abs(d.x * pin.dir.y - d.y * pin.dir.x);
					const float lobe = 0.5f + 0.5f * std::cos(2.0f * PI * t * pinnules);
					const float halfW = pin.halfWidth * std::pow(1.0f - t, 0.6f) * glm::mix(1.0f, 0.35f + 0.65f * lobe, cut);
					const float a = glm::clamp((halfW - across) / pixel + 0.5f, 0.0f, 1.0f);
					if (a > alpha)
					{
						alpha = a;
						const float midrib = 1.0f - 0.25f * glm::clamp(1.0f - across / glm::max(0.25f * halfW, 1e-5f), 0.0f, 1.0f);
						shade = glm::mix(0.78f, 1.0f, t) * midrib;
						mask = 0.0f;
					}
				}
				writeTexel(out, size, x, y, shade, mask, alpha);
			}
	}

	// A RADIAL head's PETAL: the outline by PetalShape over the strip (x = -1 .. 1 across, v = 0 .. 1 along), fine veins
	// along it, a little lighter toward the tip.
	void radialPetal(const ClutterTypeDesc& type, uint32 size, oc::vector<uint8>& out)
	{
		const float veins = 5.0f + 4.0f * rnd(type.seed, 711u);
		const float teeth = 4.0f + (float)(treeHash(type.seed, 712u) % 3u); // Fringed
		const float wave = rnd(type.seed, 713u) * 2.0f * PI;
		const float pixelX = 2.0f / (float)size, pixelV = 1.0f / (float)size;
		for (uint32 y = 0; y < size; ++y)
			for (uint32 x = 0; x < size; ++x)
			{
				const float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
				const float v = ((float)y + 0.5f) / (float)size;
				const float ax = std::abs(u);
				float halfW = 0.0f, top = 0.98f; // the half width (of 1) at v, the tip edge at u
				switch (type.petalShape)
				{
				case EPetalShape::Notched:
				{
					const float shoulder = (v - 0.82f) / 0.16f;
					halfW = glm::mix(0.4f, 0.85f, glm::smoothstep(0.0f, 0.25f, v)) * std::sqrt(glm::max(1.0f - glm::max(shoulder, 0.0f) * glm::max(shoulder, 0.0f), 0.0f));
					top = 0.98f - 0.1f * glm::max(1.0f - ax / 0.3f, 0.0f); // a V notch in the middle of the tip
					break;
				}
				case EPetalShape::Pointed:
					halfW = std::sin(PI * glm::clamp(std::pow(v, 0.7f), 0.0f, 1.0f));
					halfW = v < 0.3f ? glm::max(halfW, 0.22f) : halfW;
					break;
				case EPetalShape::Fringed:
					halfW = glm::mix(0.25f, 1.0f, glm::smoothstep(0.0f, 0.8f, v));
					top = 0.84f + 0.14f * std::pow(std::abs(std::cos(0.5f * PI * teeth * (u + 1.0f))), 0.6f);
					break;
				default: // Round
				{
					const float tip = glm::max(v - 0.5f, 0.0f) / 0.5f;
					halfW = glm::mix(0.22f, 1.0f, glm::smoothstep(0.0f, 0.45f, v)) * std::sqrt(glm::max(1.0f - tip * tip, 0.0f));
					halfW *= 1.0f - 0.05f * (0.5f + 0.5f * std::sin(v * 40.0f + wave)); // a crinkled edge
					break;
				}
				}
				const float alpha = glm::clamp(glm::min((halfW - ax) / pixelX, (top - v) / pixelV) + 0.5f, 0.0f, 1.0f);
				const float vein = std::pow(0.5f + 0.5f * std::cos(u / glm::max(halfW, 0.05f) * PI * veins), 16.0f);
				const float shade = glm::mix(0.88f, 1.0f, v) * (1.0f - 0.12f * vein * (1.0f - 0.5f * v)) * (1.0f - 0.1f * ax * ax);
				writeTexel(out, size, x, y, shade, 0.0f, alpha);
			}
	}

	// The cover of a shape from its signed distance (> 0 inside) in texels.
	float coverOf(float distanceTexels) { return glm::clamp(distanceTexels + 0.5f, 0.0f, 1.0f); }

	// An UMBEL's floret quad (yarrow): a small cluster of 5..7 florets, each five round petals around a centre (toward
	// Color2). The 24 quads of a head overlap into a lacy plate instead of one solid shape.
	void umbelFlorets(const ClutterTypeDesc& type, uint32 size, oc::vector<uint8>& out)
	{
		const uint32 count = 5u + treeHash(type.seed, 741u) % 3u;
		const float texel = 2.0f / (float)size; // the [-1, 1] square
		struct Floret { glm::vec2 centre; float radius; float turn; };
		oc::vector<Floret> florets;
		for (uint32 k = 0; k < count; ++k)
		{
			const float angle = (float)k * 2.39996323f + rnd(type.seed, 742u) * 6.2831853f;
			const float r = 0.62f * std::sqrt(((float)k + 0.5f) / (float)count);
			florets.push_back({ glm::vec2(std::cos(angle), std::sin(angle)) * r, glm::mix(0.26f, 0.34f, rnd(type.seed, 743u + k)), rnd(type.seed, 760u + k) * 6.2831853f });
		}
		for (uint32 y = 0; y < size; ++y)
			for (uint32 x = 0; x < size; ++x)
			{
				const glm::vec2 p(((float)x + 0.5f) / (float)size * 2.0f - 1.0f, ((float)y + 0.5f) / (float)size * 2.0f - 1.0f);
				float alpha = 0.0f, shade = 0.0f, mask = 0.0f;
				for (const Floret& f : florets)
				{
					const glm::vec2 d = p - f.centre;
					const float r = glm::length(d);
					const float theta = std::atan2(d.y, d.x) + f.turn;
					const float edge = f.radius * (0.55f + 0.45f * std::abs(std::cos(2.5f * theta))); // five lobes
					const float cover = coverOf((edge - r) / texel);
					if (cover > alpha)
					{
						alpha = cover;
						const float inner = r / f.radius;
						mask = inner < 0.28f ? 1.0f : 0.0f;
						shade = inner < 0.28f ? 0.8f : glm::mix(0.8f, 1.0f, glm::clamp(inner * 1.4f, 0.0f, 1.0f));
					}
				}
				writeTexel(out, size, x, y, shade, mask, alpha);
			}
	}

	// A SPIKE's floret (lupine, lavender, heather): a pea flower from the stalk (v = 0) outward - the KEEL and wings
	// near the base (toward Color2), the round BANNER beyond them, a short stalk.
	void spikeFloret(const ClutterTypeDesc& type, uint32 size, oc::vector<uint8>& out)
	{
		(void)type;
		const float texel = 2.0f / (float)size;
		for (uint32 y = 0; y < size; ++y)
			for (uint32 x = 0; x < size; ++x)
			{
				const glm::vec2 p(((float)x + 0.5f) / (float)size * 2.0f - 1.0f, ((float)y + 0.5f) / (float)size * 2.0f - 1.0f);
				float alpha = 0.0f, shade = 0.0f, mask = 0.0f;
				// The stalk.
				if (p.y < -0.6f)
				{
					alpha = coverOf((0.07f - std::abs(p.x)) / texel);
					shade = 0.55f;
				}
				// The banner: a round petal, a slight notch at its top, darker toward its centre.
				const glm::vec2 b = (p - glm::vec2(0.0f, 0.3f)) / glm::vec2(0.85f, 0.65f);
				const float notch = 0.12f * glm::max(1.0f - std::abs(p.x) / 0.2f, 0.0f) * (p.y > 0.6f ? 1.0f : 0.0f);
				const float banner = coverOf((1.0f - glm::length(b) - notch) * 0.65f / texel);
				if (banner > alpha)
				{
					alpha = banner;
					shade = glm::mix(0.7f, 1.0f, glm::clamp(glm::length(b), 0.0f, 1.0f));
					mask = 0.0f;
				}
				// The keel and wings in front of it.
				const glm::vec2 k = (p - glm::vec2(0.0f, -0.3f)) / glm::vec2(0.45f, 0.42f);
				const float keel = coverOf((1.0f - glm::length(k)) * 0.42f / texel);
				if (keel > 0.0f && keel >= alpha * 0.5f)
				{
					alpha = glm::max(alpha, keel);
					shade = glm::mix(0.8f, 1.0f, glm::clamp(1.0f - glm::dot(k, k), 0.0f, 1.0f));
					mask = 1.0f;
				}
				writeTexel(out, size, x, y, shade, mask, alpha);
			}
	}

	// A BELL's floret (foxglove, bluebell): a tube from its stalk (v = 0) widening to a flared mouth with five scalloped
	// lobes at the tip, shaded round across.
	void bellFloret(const ClutterTypeDesc& type, uint32 size, oc::vector<uint8>& out)
	{
		(void)type;
		const float pixelX = 2.0f / (float)size, pixelV = 1.0f / (float)size;
		for (uint32 y = 0; y < size; ++y)
			for (uint32 x = 0; x < size; ++x)
			{
				const float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
				const float v = ((float)y + 0.5f) / (float)size;
				const float halfW = glm::mix(0.3f, 0.68f, glm::smoothstep(0.0f, 0.7f, v)) + 0.3f * glm::smoothstep(0.72f, 1.0f, v);
				const float top = 0.88f + 0.1f * std::sqrt(std::abs(std::cos(0.5f * 3.14159265f * 2.5f * u)));
				const float alpha = glm::clamp(glm::min((halfW - std::abs(u)) / pixelX, (top - v) / pixelV) + 0.5f, 0.0f, 1.0f);
				const float across = std::abs(u) / glm::max(halfW, 0.05f);
				const float shade = glm::mix(0.72f, 1.0f, 1.0f - across * across) * glm::mix(0.8f, 1.0f, v);
				writeTexel(out, size, x, y, shade, 0.0f, alpha);
			}
	}

	float segmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b)
	{
		const glm::vec2 ab = b - a;
		const float t = glm::clamp(glm::dot(p - a, ab) / glm::max(glm::dot(ab, ab), 1e-8f), 0.0f, 1.0f);
		return glm::length(p - (a + ab * t));
	}

	// A GRAIN's EAR and its AWNS on one strip (CLUTTER_GRAIN_STRIP_WIDEN x the ear's width: the ear in its middle): the
	// ear over the first 1 / (1 + 2 x Awns) of it - spikelets alternating either side of the rachis, smaller toward the
	// top - and 2..3 awns (toward Color2) from each spikelet's tip, 75..100 % of the awn length long, FANNING OUT: each
	// at its own angle from upright, even over -32..32 degrees (the lower spikelets' the widest),
	// set in the strip's physical proportions so the angle holds; an awn that would leave the strip is shortened.
	// `Ear Dense` (wheat): that. `Ear Slender` (barley): more, smaller spikelets, THREE awns each (the triplets), long
	// and nearly parallel (-13..13 degrees) - a silky brush. Drawn in PIXELS (x = -1 .. 1 across), so the awns stay ~2
	// texels thick.
	void grainEar(const ClutterTypeDesc& type, uint32 size, oc::vector<uint8>& out)
	{
		const bool slender = type.ear == EGrainEar::Slender;
		const float awns = glm::clamp(type.spots, 0.0f, 1.0f);
		const float vEar = 1.0f / (1.0f + 2.0f * awns);
		const float earX = 1.0f / RendererVKLayout::CLUTTER_GRAIN_STRIP_WIDEN; // the ear's half width across the strip
		// The strip's half width / its length (the VS: WIDEN x PetalWidth x HeadSize wide, (1 + 2 Awns) x HeadSize long).
		const float halfWidthPerLength = 0.5f * RendererVKLayout::CLUTTER_GRAIN_STRIP_WIDEN * glm::max(type.petalWidth, 0.01f) / (1.0f + 2.0f * awns);
		const uint32 count = slender ? 24u + treeHash(type.seed, 721u) % 9u : 14u + treeHash(type.seed, 721u) % 7u; // spikelets, both sides
		const float minAngle = slender ? 3.0f : 4.0f, angleRange = slender ? 10.0f : 28.0f;
		const float half = 0.5f * (float)size;
		struct Spikelet { glm::vec2 centre; glm::vec2 radius; glm::vec2 tip; };
		struct Awn { glm::vec2 from; glm::vec2 to; };
		oc::vector<Spikelet> spikelets;
		oc::vector<Awn> awnLines;
		for (uint32 k = 0; k < count; ++k)
		{
			const float s = ((float)k + 0.5f) / (float)count;
			const float side = (k & 1u) ? 1.0f : -1.0f;
			const float taper = slender ? glm::mix(1.0f, 0.75f, s) : glm::mix(1.0f, 0.55f, s);
			Spikelet sp;
			sp.centre = glm::vec2(side * 0.32f * taper * earX, vEar * (0.05f + 0.88f * s));
			sp.radius = glm::vec2(0.5f * taper * earX, (slender ? 2.0f : 1.6f) * vEar / (float)count);
			sp.tip = sp.centre + glm::vec2(side * 0.15f * taper * earX, sp.radius.y);
			spikelets.push_back(sp);
			const uint32 perSpikelet = slender ? 3u : 2u + treeHash(type.seed, 750u + k) % 2u;
			for (uint32 a = 0; a < perSpikelet && awns > 0.0f; ++a)
			{
				const uint32 h = treeHash(type.seed, 800u + k * 4u + a);
				const float length = (1.0f - vEar) * glm::mix(slender ? 0.85f : 0.75f, 1.0f, treeHash01(h)); // in strip lengths
				// The angle from upright: EVEN over the whole fan (-max .. max, either side of upright, not split by the
				// spikelet's side - that left a gap down the middle, a V), narrower high on the ear.
				const float signedAngle = (2.0f * treeHash01(treeHash(h, 1u)) - 1.0f) * (minAngle + angleRange) * glm::mix(1.0f, 0.6f, s);
				const float sign = signedAngle < 0.0f ? -1.0f : 1.0f;
				const float angle = glm::radians(std::abs(signedAngle));
				// Along v: cos x length; across (x units of the half width): sin x length / (half width / length).
				glm::vec2 delta(sign * std::sin(angle) * length / halfWidthPerLength, std::cos(angle) * length);
				const float room = (sign > 0.0f ? 0.97f - sp.tip.x : sp.tip.x + 0.97f);
				const float fit = glm::min(glm::min(1.0f, room / glm::max(std::abs(delta.x), 1e-5f)), (0.995f - sp.tip.y) / glm::max(delta.y, 1e-5f));
				awnLines.push_back({ sp.tip, sp.tip + delta * glm::max(fit, 0.0f) });
			}
		}
		for (uint32 y = 0; y < size; ++y)
			for (uint32 x = 0; x < size; ++x)
			{
				const float u = ((float)x + 0.5f) / half - 1.0f;
				const float v = ((float)y + 0.5f) / (float)size;
				const glm::vec2 px((float)x + 0.5f, (float)y + 0.5f); // pixels
				float alpha = 0.0f, shade = 0.0f, mask = 0.0f;
				// THE AWNS (under the spikelets): a line ~2 texels wide each.
				for (const Awn& awn : awnLines)
				{
					const glm::vec2 a((awn.from.x + 1.0f) * half, awn.from.y * (float)size);
					const glm::vec2 b((awn.to.x + 1.0f) * half, awn.to.y * (float)size);
					const float cover = glm::clamp(1.0f - segmentDistance(px, a, b) + 0.5f, 0.0f, 1.0f);
					if (cover > alpha)
					{
						alpha = cover;
						shade = 0.95f;
						mask = 1.0f;
					}
				}
				// THE RACHIS up the ear's middle: as wide as the stalk's top, in the stalk's colour.
				if (v < vEar && std::abs(u) < RendererVKLayout::CLUTTER_GRAIN_STALK_HALF_WIDTH / (0.5f * RendererVKLayout::CLUTTER_GRAIN_STRIP_WIDEN) && alpha < 1.0f)
				{
					alpha = 1.0f;
					shade = 1.0f;
					mask = 1.0f;
				}
				// THE SPIKELETS: rounded, with a darker rim and a highlight.
				for (const Spikelet& sp : spikelets)
				{
					const glm::vec2 d = (glm::vec2(u, v) - sp.centre) / sp.radius;
					const float r2 = glm::dot(d, d);
					// The rim's width in the ellipse's units: about a texel.
					const float edge = (1.0f - std::sqrt(r2)) * glm::min(sp.radius.x * half, sp.radius.y * (float)size);
					const float cover = glm::clamp(edge + 0.5f, 0.0f, 1.0f);
					if (cover > 0.0f && (cover >= alpha || mask > 0.5f))
					{
						alpha = glm::max(alpha, cover);
						shade = glm::mix(0.62f, 1.0f, glm::clamp(1.0f - r2, 0.0f, 1.0f)) * (1.0f - 0.15f * glm::clamp(d.x * (sp.centre.x > 0.0f ? -1.0f : 1.0f), 0.0f, 1.0f));
						mask = 0.0f;
					}
				}
				writeTexel(out, size, x, y, shade, mask, alpha);
			}
	}

	// `Ear Panicle` (oat): no dense ear - a loose PANICLE over the whole strip (after a wild oat): a stout rachis tapering
	// up, 4..5 levels of 1..2 thin PEDICELS that arch out and up, then over (a Bezier), and from each end ONE SPIKELET
	// hanging - two long, pointed glumes opening in a narrow V (Color) - plus one from the rachis's tip: 7..11 in all.
	// Some spikelets trail a bent awn. The rachis and pedicels toward Color2. Laid out in PHYSICAL units (x = the
	// strip's half width x u) so the angles hold; the glumes' edges antialiased over the coarser texel axis.
	void grainPanicle(const ClutterTypeDesc& type, uint32 size, oc::vector<uint8>& out)
	{
		const float awns = glm::clamp(type.spots, 0.0f, 1.0f);
		const float hwpl = 0.5f * RendererVKLayout::CLUTTER_GRAIN_STRIP_WIDEN * glm::max(type.petalWidth, 0.01f) / (1.0f + 2.0f * awns);
		const float half = 0.5f * (float)size;
		const auto toPx = [&](glm::vec2 p) { return glm::vec2((p.x / hwpl + 1.0f) * half, p.y * (float)size); };
		// Texels per physical unit on the coarser axis (an edge's antialiasing band).
		const float texelsPerUnit = glm::min(half / hwpl, (float)size);
		struct Line { glm::vec2 a; glm::vec2 b; float halfWidth; }; // pixels
		struct Glume { glm::vec2 base; glm::vec2 dir; float length; float halfWidth; }; // physical
		oc::vector<Line> lines;
		oc::vector<Glume> glumes;
		const auto line = [&](glm::vec2 a, glm::vec2 b, float halfWidth) { lines.push_back({ toPx(a), toPx(b), halfWidth }); };
		const float spikeletLen = 0.13f; // in strip lengths
		const auto hang = [&](glm::vec2 at, glm::vec2 down, uint32 h)
		{
			// Two glumes in a narrow V about `down`, one a little longer.
			const float open = glm::radians(glm::mix(6.0f, 12.0f, treeHash01(h)));
			for (int g = -1; g <= 1; g += 2)
			{
				const float a = (float)g * open;
				const glm::vec2 dir(down.x * std::cos(a) - down.y * std::sin(a), down.x * std::sin(a) + down.y * std::cos(a));
				glumes.push_back({ at, dir, spikeletLen * (g < 0 ? 1.0f : 0.88f), spikeletLen * 0.11f });
			}
			if (treeHash01(treeHash(h, 1u)) < 0.45f) // a bent awn from its middle: out, kinked, then down
			{
				const float side = treeHash01(treeHash(h, 2u)) < 0.5f ? -1.0f : 1.0f;
				const glm::vec2 mid = at + down * (0.5f * spikeletLen);
				const glm::vec2 knee = mid + glm::vec2(side * 0.35f * spikeletLen, -0.15f * spikeletLen);
				line(mid, knee, 1.0f);
				line(knee, knee + glm::vec2(side * 0.1f * spikeletLen, -0.7f * spikeletLen), 1.0f);
			}
		};
		// THE RACHIS: at its base as wide as the stalk's top (CLUTTER_GRAIN_STALK_HALF_WIDTH x the ear's width, the strip
		// WIDEN x that), tapering to half of it up (pixels of half width).
		const float rachisBase = RendererVKLayout::CLUTTER_GRAIN_STALK_HALF_WIDTH / (0.5f * RendererVKLayout::CLUTTER_GRAIN_STRIP_WIDEN) * half;
		constexpr int RACHIS_SEGMENTS = 4;
		for (int s = 0; s < RACHIS_SEGMENTS; ++s)
		{
			const float y0 = 0.96f * (float)s / RACHIS_SEGMENTS, y1 = 0.96f * (float)(s + 1) / RACHIS_SEGMENTS;
			line(glm::vec2(0.0f, y0), glm::vec2(0.0f, y1), rachisBase * glm::mix(1.0f, 0.5f, (float)s / (RACHIS_SEGMENTS - 1)));
		}
		hang(glm::vec2(0.0f, 0.96f), glm::normalize(glm::vec2(0.15f, -1.0f)), treeHash(type.seed, 770u));
		const uint32 levels = 4u + treeHash(type.seed, 771u) % 2u;
		for (uint32 l = 0; l < levels; ++l)
		{
			const float f = (float)l / (float)(levels - 1u);
			const glm::vec2 base(0.0f, glm::mix(0.3f, 0.86f, f));
			const uint32 branches = 1u + treeHash(type.seed, 772u + l) % 2u;
			for (uint32 b = 0; b < branches; ++b)
			{
				const uint32 h = treeHash(type.seed, 900u + l * 4u + b);
				const float side = ((b + l) & 1u) ? 1.0f : -1.0f;
				const float angle = glm::radians(glm::mix(25.0f, 50.0f, treeHash01(h)));
				// The pedicel: out and up to its control point, then over to its end (out, a little down); its reach
				// across stays inside 85 % of the strip's half width.
				const float wantLen = glm::mix(0.3f, 0.14f, f) * glm::mix(0.85f, 1.1f, treeHash01(treeHash(h, 3u)));
				const float reach = std::sin(angle) + 0.3f;
				const float len = glm::min(wantLen, 0.85f * hwpl / reach);
				const glm::vec2 control = base + glm::vec2(side * std::sin(angle), std::cos(angle)) * len;
				const glm::vec2 end = control + glm::vec2(side * 0.3f * len, -0.2f * len);
				constexpr int PEDICEL_SEGMENTS = 6;
				glm::vec2 prev = base;
				for (int s = 1; s <= PEDICEL_SEGMENTS; ++s)
				{
					const float t = (float)s / PEDICEL_SEGMENTS;
					const glm::vec2 p = (1.0f - t) * (1.0f - t) * base + 2.0f * (1.0f - t) * t * control + t * t * end;
					line(prev, p, 1.6f);
					prev = p;
				}
				// The spikelet hangs along the pedicel's end direction, turned toward the ground.
				const glm::vec2 tangent = glm::normalize(end - control);
				hang(end, glm::normalize(tangent * 0.35f + glm::vec2(0.0f, -1.0f)), treeHash(h, 4u));
			}
		}
		for (uint32 y = 0; y < size; ++y)
			for (uint32 x = 0; x < size; ++x)
			{
				const glm::vec2 px((float)x + 0.5f, (float)y + 0.5f);
				const glm::vec2 phys((((float)x + 0.5f) / half - 1.0f) * hwpl, ((float)y + 0.5f) / (float)size);
				float alpha = 0.0f, shade = 0.0f, mask = 0.0f;
				for (const Line& ln : lines)
				{
					const float cover = coverOf(ln.halfWidth - segmentDistance(px, ln.a, ln.b));
					if (cover > alpha)
					{
						alpha = cover;
						shade = 1.0f; // Color2 as is: the stalk's colour
						mask = 1.0f;
					}
				}
				// THE GLUMES: pointed at both ends, widest at a third of their length, a darker midrib.
				for (const Glume& g : glumes)
				{
					const glm::vec2 d = phys - g.base;
					const float t = glm::dot(d, g.dir) / g.length;
					if (t <= 0.0f || t >= 1.0f)
						continue;
					const float across = std::abs(d.x * g.dir.y - d.y * g.dir.x);
					const float w = g.halfWidth * std::sin(3.14159265f * std::pow(t, 0.6f));
					const float cover = coverOf((w - across) * texelsPerUnit);
					if (cover > 0.0f && (cover >= alpha || mask > 0.5f))
					{
						alpha = glm::max(alpha, cover);
						const float rib = 1.0f - 0.2f * glm::clamp(1.0f - across / glm::max(0.3f * w, 1e-5f), 0.0f, 1.0f);
						shade = glm::mix(0.75f, 1.0f, t) * rib;
						mask = 0.0f;
					}
				}
				writeTexel(out, size, x, y, shade, mask, alpha);
			}
	}
}

namespace Procedural
{
	bool generateClutterTexture(const ClutterTypeDesc& type, uint32& outSize, oc::vector<uint8>& outRgba)
	{
		if (type.kind != EClutterKind::Flower)
			return false;
		switch (type.head)
		{
		case EFlowerHead::Fern:
			outSize = FERN_SIZE;
			outRgba.assign((size_t)outSize * outSize * 4, 0);
			fernFrond(type, outSize, outRgba);
			return true;
		case EFlowerHead::Radial:
			if (type.petalShape == EPetalShape::None)
				return false;
			outSize = PETAL_SIZE;
			outRgba.assign((size_t)outSize * outSize * 4, 0);
			radialPetal(type, outSize, outRgba);
			return true;
		case EFlowerHead::Grain:
			outSize = GRAIN_SIZE;
			outRgba.assign((size_t)outSize * outSize * 4, 0);
			if (type.ear == EGrainEar::Panicle)
				grainPanicle(type, outSize, outRgba);
			else
				grainEar(type, outSize, outRgba);
			return true;
		case EFlowerHead::Umbel:
		case EFlowerHead::Spike:
		case EFlowerHead::Bell:
			if (type.petalShape == EPetalShape::None)
				return false;
			outSize = PETAL_SIZE;
			outRgba.assign((size_t)outSize * outSize * 4, 0);
			if (type.head == EFlowerHead::Umbel)
				umbelFlorets(type, outSize, outRgba);
			else if (type.head == EFlowerHead::Spike)
				spikeFloret(type, outSize, outRgba);
			else
				bellFloret(type, outSize, outRgba);
			return true;
		default:
			return false;
		}
	}
}

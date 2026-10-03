module Procedural;

import Core;
import Core.glm;

import :TreeGenerator;
import :TreeImpostor;

namespace
{
	using namespace Procedural;

	constexpr uint32 SUPERSAMPLE = 2; // per axis; coverage alpha comes from the 2x2 subsamples
	constexpr uint32 BAKE_VERSION = 7; // bump when the bake changes: cached atlases re-bake

	glm::vec2 signNotZero(glm::vec2 v) { return glm::vec2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f); }

	glm::vec3 sampleWrap(const TreeBakeImage& image, glm::vec2 uv, float& outAlpha)
	{
		const float u = uv.x - std::floor(uv.x), v = uv.y - std::floor(uv.y);
		const uint32 x = glm::min((uint32)(u * (float)image.size), image.size - 1);
		const uint32 y = glm::min((uint32)(v * (float)image.size), image.size - 1);
		const uint8* p = &image.rgba[((size_t)y * image.size + x) * 4];
		outAlpha = (float)p[3] * (1.0f / 255.0f);
		return glm::vec3(p[0], p[1], p[2]) * (1.0f / 255.0f);
	}

	// One orthographic view being baked (supersampled): `centre` + right/up/dir basis, `halfExtent` along
	// right / up mapped onto the width / height.
	struct FrameTarget
	{
		uint32 width = 0;
		uint32 height = 0;
		oc::vector<float> depth;
		oc::vector<glm::vec3> color;
		oc::vector<glm::vec3> normal;

		void clear(uint32 w, uint32 h)
		{
			width = w;
			height = h;
			depth.assign((size_t)w * h, -FLT_MAX);
			color.assign((size_t)w * h, glm::vec3(0.0f));
			normal.assign((size_t)w * h, glm::vec3(0.0f));
		}
	};

	struct ViewBasis
	{
		glm::vec3 centre;
		glm::vec3 right;
		glm::vec3 up;
		glm::vec3 dir;       // toward the viewer
		glm::vec2 halfExtent; // along right / up
	};

	float edge(glm::vec2 a, glm::vec2 b, glm::vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

	// Rasterizes one mesh into the view: back faces culled against the view direction (every piece mesh is
	// closed or double-sided), z = distance toward the viewer (larger wins), alpha-tested when `alphaTest`.
	void rasterMesh(FrameTarget& target, const TreeMesh& mesh, const ViewBasis& view, const TreeBakeImage& image, bool alphaTest)
	{
		if (image.size == 0)
			return;
		const glm::vec3& centre = view.centre;
		const glm::vec3& right = view.right;
		const glm::vec3& up = view.up;
		const glm::vec3& dir = view.dir;
		const glm::vec2 scale = glm::vec2((float)target.width, (float)target.height) / glm::max(view.halfExtent, glm::vec2(1e-4f));
		for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
		{
			const uint32 i0 = mesh.indices[t], i1 = mesh.indices[t + 1], i2 = mesh.indices[t + 2];
			const glm::vec3& p0 = mesh.positions[i0];
			const glm::vec3& p1 = mesh.positions[i1];
			const glm::vec3& p2 = mesh.positions[i2];
			if (glm::dot(glm::cross(p1 - p0, p2 - p0), dir) <= 0.0f)
				continue;
			glm::vec2 sp[3];
			float z[3];
			const glm::vec3* pts[3] = { &p0, &p1, &p2 };
			for (int k = 0; k < 3; ++k)
			{
				const glm::vec3 d = *pts[k] - centre;
				sp[k] = glm::vec2((glm::dot(d, right) * 0.5f) * scale.x + (float)target.width * 0.5f,
					(float)target.height * 0.5f - (glm::dot(d, up) * 0.5f) * scale.y);
				z[k] = glm::dot(d, dir);
			}
			const float area = edge(sp[0], sp[1], sp[2]);
			if (glm::abs(area) < 1e-8f)
				continue;
			const float invArea = 1.0f / area;
			const int x0 = glm::max((int)std::floor(glm::min(sp[0].x, glm::min(sp[1].x, sp[2].x))), 0);
			const int y0 = glm::max((int)std::floor(glm::min(sp[0].y, glm::min(sp[1].y, sp[2].y))), 0);
			const int x1 = glm::min((int)std::ceil(glm::max(sp[0].x, glm::max(sp[1].x, sp[2].x))), (int)target.width - 1);
			const int y1 = glm::min((int)std::ceil(glm::max(sp[0].y, glm::max(sp[1].y, sp[2].y))), (int)target.height - 1);
			for (int y = y0; y <= y1; ++y)
			{
				for (int x = x0; x <= x1; ++x)
				{
					const glm::vec2 p((float)x + 0.5f, (float)y + 0.5f);
					const float w0 = edge(sp[1], sp[2], p) * invArea;
					const float w1 = edge(sp[2], sp[0], p) * invArea;
					const float w2 = 1.0f - w0 - w1;
					if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
						continue;
					const size_t idx = (size_t)y * target.width + (size_t)x;
					const float depth = w0 * z[0] + w1 * z[1] + w2 * z[2];
					if (depth <= target.depth[idx])
						continue;
					const glm::vec2 uv = glm::vec2(mesh.texCoords[i0]) * w0 + glm::vec2(mesh.texCoords[i1]) * w1 + glm::vec2(mesh.texCoords[i2]) * w2;
					float alpha = 1.0f;
					const glm::vec3 color = sampleWrap(image, uv, alpha);
					if (alphaTest && alpha < 0.5f)
						continue;
					target.depth[idx] = depth;
					target.color[idx] = color;
					target.normal[idx] = mesh.normals[i0] * w0 + mesh.normals[i1] * w1 + mesh.normals[i2] * w2;
				}
			}
		}
	}

	uint8 toByte(float v) { return (uint8)(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

	// Resolves the supersampled view into the output images at (offsetX, offsetY) of an `outWidth`-wide RGBA8
	// pair: coverage alpha, averaged colour / normal / depth of the covered subsamples. Normals go into the
	// view's tangent space (x right, y up, z toward the viewer; never facing away), depth / depthScale into the
	// normal image's alpha. Empty texels take the view's mean colour, so filtering never darkens the silhouette.
	//
	// `volumeCentre` (optional) switches to VOLUME normals: each texel's normal is bent by `volumeBend` toward
	// the direction out of the clump's centre (its 3D position comes from the baked depth), and keeps its sign
	// - normals may face away from the view - so a flat card shades like a round clump whatever side the sun
	// is on. Without it the normals are clamped toward the viewer (the impostor frames).
	void resolveView(const FrameTarget& target, const ViewBasis& view, float depthScale,
		oc::vector<uint8>& outAlbedo, oc::vector<uint8>& outNormal, uint32 outWidth, uint32 offsetX, uint32 offsetY,
		const glm::vec3* volumeCentre = nullptr, float volumeBend = 0.0f)
	{
		glm::vec3 meanColor(0.0f);
		uint32 meanCount = 0;
		for (size_t k = 0; k < target.depth.size(); ++k)
			if (target.depth[k] > -FLT_MAX)
			{
				meanColor += target.color[k];
				++meanCount;
			}
		meanColor = meanCount > 0 ? meanColor / (float)meanCount : glm::vec3(0.3f);

		const uint32 w = target.width / SUPERSAMPLE, h = target.height / SUPERSAMPLE;
		for (uint32 y = 0; y < h; ++y)
		{
			for (uint32 x = 0; x < w; ++x)
			{
				glm::vec3 color(0.0f), normal(0.0f);
				float depth = 0.0f;
				uint32 covered = 0;
				for (uint32 sy = 0; sy < SUPERSAMPLE; ++sy)
					for (uint32 sx = 0; sx < SUPERSAMPLE; ++sx)
					{
						const size_t k = (size_t)(y * SUPERSAMPLE + sy) * target.width + (x * SUPERSAMPLE + sx);
						if (target.depth[k] <= -FLT_MAX)
							continue;
						color += target.color[k];
						normal += target.normal[k];
						depth += target.depth[k];
						++covered;
					}
				const size_t out = (((size_t)offsetY + y) * outWidth + offsetX + x) * 4;
				if (covered == 0)
				{
					outAlbedo[out + 0] = toByte(meanColor.x);
					outAlbedo[out + 1] = toByte(meanColor.y);
					outAlbedo[out + 2] = toByte(meanColor.z);
					outAlbedo[out + 3] = 0;
					outNormal[out + 0] = 128;
					outNormal[out + 1] = 128;
					outNormal[out + 2] = 255;
					outNormal[out + 3] = 128; // depth 0: on the card plane (the mips average it into the edges)
					continue;
				}
				color /= (float)covered;
				depth /= (float)covered;
				glm::vec3 n = glm::dot(normal, normal) > 1e-12f ? glm::normalize(normal) : view.dir;
				glm::vec3 nt;
				if (volumeCentre)
				{
					const glm::vec2 xy(((float)x + 0.5f) / (float)w * 2.0f - 1.0f, 1.0f - ((float)y + 0.5f) / (float)h * 2.0f);
					const glm::vec3 p = view.centre + view.right * (xy.x * view.halfExtent.x) + view.up * (xy.y * view.halfExtent.y) + view.dir * depth;
					const glm::vec3 outward = p - *volumeCentre;
					if (glm::dot(outward, outward) > 1e-8f)
					{
						const glm::vec3 bent = glm::mix(n, glm::normalize(outward), volumeBend);
						if (glm::dot(bent, bent) > 1e-8f)
							n = glm::normalize(bent);
					}
					nt = glm::vec3(glm::dot(n, view.right), glm::dot(n, view.up), glm::dot(n, view.dir));
				}
				else
					nt = glm::normalize(glm::vec3(glm::dot(n, view.right), glm::dot(n, view.up), glm::max(glm::dot(n, view.dir), 0.05f)));
				outAlbedo[out + 0] = toByte(color.x);
				outAlbedo[out + 1] = toByte(color.y);
				outAlbedo[out + 2] = toByte(color.z);
				outAlbedo[out + 3] = toByte((float)covered / (float)(SUPERSAMPLE * SUPERSAMPLE));
				outNormal[out + 0] = toByte(nt.x * 0.5f + 0.5f);
				outNormal[out + 1] = toByte(nt.y * 0.5f + 0.5f);
				outNormal[out + 2] = toByte(nt.z * 0.5f + 0.5f);
				outNormal[out + 3] = toByte(depth / glm::max(depthScale, 1e-4f) * 0.5f + 0.5f);
			}
		}
	}
}

namespace Procedural
{
	glm::vec2 impostorOctEncode(glm::vec3 d)
	{
		d /= glm::abs(d.x) + glm::abs(d.y) + glm::abs(d.z);
		glm::vec2 p(d.x, d.z);
		if (d.y < 0.0f)
			p = (1.0f - glm::abs(glm::vec2(p.y, p.x))) * signNotZero(p);
		return p;
	}

	glm::vec3 impostorOctDecode(glm::vec2 p)
	{
		glm::vec3 d(p.x, 1.0f - glm::abs(p.x) - glm::abs(p.y), p.y);
		if (d.y < 0.0f)
		{
			const glm::vec2 xz = (1.0f - glm::abs(glm::vec2(d.z, d.x))) * signNotZero(glm::vec2(d.x, d.z));
			d.x = xz.x;
			d.z = xz.y;
		}
		return glm::normalize(d);
	}

	void impostorFrameBasis(const glm::vec3& dir, glm::vec3& outRight, glm::vec3& outUp)
	{
		const glm::vec3 ref = glm::abs(dir.y) > 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		outRight = glm::normalize(glm::cross(ref, dir));
		outUp = glm::cross(dir, outRight);
	}

	TreeImpostorBounds impostorBounds(const TreePiece& piece)
	{
		glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
		for (const TreeMesh* mesh : { &piece.bark[0], &piece.leaves[0] })
			for (const glm::vec3& p : mesh->positions)
			{
				mn = glm::min(mn, p);
				mx = glm::max(mx, p);
			}
		TreeImpostorBounds bounds;
		if (mn.x > mx.x)
			return bounds;
		bounds.centre = (mn + mx) * 0.5f;
		float r2 = 0.0f;
		for (const TreeMesh* mesh : { &piece.bark[0], &piece.leaves[0] })
			for (const glm::vec3& p : mesh->positions)
				r2 = glm::max(r2, glm::dot(p - bounds.centre, p - bounds.centre));
		bounds.radius = glm::max(std::sqrt(r2), 1e-3f);
		return bounds;
	}

	uint32 impostorHash(const TreePiece& piece, uint32 frames, uint32 frameSize)
	{
		uint32 h = treeHash(BAKE_VERSION, treeHash(frames, frameSize));
		for (const TreeMesh* mesh : { &piece.bark[0], &piece.leaves[0] })
		{
			h = treeHash(h, mesh->numVertices());
			h = treeHash(h, (uint32)mesh->indices.size());
			// A strided sample of the positions, quantized to millimetres: any shape change moves some of them.
			const size_t step = glm::max<size_t>(mesh->positions.size() / 64, 1);
			for (size_t i = 0; i < mesh->positions.size(); i += step)
			{
				const glm::ivec3 q = glm::ivec3(glm::round(mesh->positions[i] * 1000.0f));
				h = treeHash(h, treeHash((uint32)q.x, treeHash((uint32)q.y, (uint32)q.z)));
			}
		}
		return h;
	}

	void bakeImpostor(const TreePiece& piece, const TreeImpostorBounds& bounds, const TreeBakeImage& bark, const TreeBakeImage& leaves,
		uint32 frames, uint32 frameSize, oc::vector<uint8>& outAlbedo, oc::vector<uint8>& outNormal)
	{
		const uint32 atlas = frames * frameSize;
		outAlbedo.assign((size_t)atlas * atlas * 4, 0);
		outNormal.assign((size_t)atlas * atlas * 4, 0);
		FrameTarget target;
		for (uint32 j = 0; j < frames; ++j)
		{
			for (uint32 i = 0; i < frames; ++i)
			{
				const glm::vec2 p = (glm::vec2((float)i, (float)j) + 0.5f) / (float)frames * 2.0f - 1.0f;
				const glm::vec3 dir = impostorOctDecode(p);
				glm::vec3 right, up;
				impostorFrameBasis(dir, right, up);

				const ViewBasis view{ bounds.centre, right, up, dir, glm::vec2(bounds.radius) };
				target.clear(frameSize * SUPERSAMPLE, frameSize * SUPERSAMPLE);
				rasterMesh(target, piece.bark[0], view, bark, false);
				rasterMesh(target, piece.leaves[0], view, leaves, true);
				resolveView(target, view, bounds.radius, outAlbedo, outNormal, atlas, i * frameSize, j * frameSize);
			}
		}
	}

	void bakeTreeDensity(const TreePiece& piece, uint32 res, float leafCoverage, oc::vector<float>& out, TreeBillboardBox& outBox)
	{
		outBox = billboardBox(piece);
		out.assign((size_t)res * res * res, 0.0f);
		const glm::vec3 size = glm::max(outBox.max - outBox.min, glm::vec3(1e-3f));
		const glm::vec3 voxel = size / (float)res;
		const float invVoxelVolume = 1.0f / (voxel.x * voxel.y * voxel.z);
		const float minVoxel = glm::min(glm::min(voxel.x, voxel.y), voxel.z);
		auto splat = [&](const TreeMesh& mesh, float blockPerArea)
		{
			for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
			{
				const glm::vec3 a = mesh.positions[mesh.indices[t]];
				const glm::vec3 b = mesh.positions[mesh.indices[t + 1]];
				const glm::vec3 c = mesh.positions[mesh.indices[t + 2]];
				const float area = 0.5f * glm::length(glm::cross(b - a, c - a));
				if (area <= 0.0f)
					continue;
				// A barycentric grid of n(n+1)/2 points, fine enough that every voxel the triangle crosses gets some.
				const float longest = glm::max(glm::max(glm::length(b - a), glm::length(c - b)), glm::length(a - c));
				const uint32 n = glm::clamp((uint32)std::ceil(longest / (0.5f * minVoxel)), 1u, 32u);
				const float share = area * blockPerArea * invVoxelVolume / (float)(n * (n + 1) / 2);
				for (uint32 i = 0; i < n; ++i)
					for (uint32 j = 0; i + j < n; ++j)
					{
						const float u = ((float)i + 1.0f / 3.0f) / (float)n, v = ((float)j + 1.0f / 3.0f) / (float)n;
						const glm::vec3 p = a + (b - a) * u + (c - a) * v;
						const glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor((p - outBox.min) / voxel)), glm::ivec3(0), glm::ivec3((int)res - 1));
						out[(size_t)cell.x + res * ((size_t)cell.y + res * (size_t)cell.z)] += share;
					}
			}
		};
		// Leaves only: the bark (0.25 per area before) put a trunk's whole surface into one thin column of voxels - a dense
		// vertical line per tree, darkened by the volume's self-shadow and interior taps: dashed vertical streaks through
		// the far blobs. A trunk is far below a pixel at the volume's distances.
		splat(piece.leaves[0], 0.5f * leafCoverage);
	}

	TreeBillboardBox billboardBox(const TreePiece& piece)
	{
		glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
		for (const TreeMesh* mesh : { &piece.bark[0], &piece.leaves[0] })
			for (const glm::vec3& p : mesh->positions)
			{
				mn = glm::min(mn, p);
				mx = glm::max(mx, p);
			}
		if (mn.x > mx.x)
			return {};
		return { mn, glm::max(mx, mn + 1e-3f) };
	}

	void billboardViews(const TreeBillboardBox& box, TreeBillboardView& outSide, TreeBillboardView& outTop)
	{
		const glm::vec3 centre = (box.min + box.max) * 0.5f;
		const glm::vec3 half = (box.max - box.min) * 0.5f;
		// Both card PLANES pass through the branch axis (module x = 0 / z = 0, where the root attaches to the
		// trunk) - not through the box centre, which the leaf spread pushes off the branch (the branch image
		// then floated beside its attach point). In-plane they still cover the whole box. The bake projects
		// orthographically along the card normal, so the plane offset never changes the image.
		// Side: the vertical card, seen from +X; u along the branch (+Y), v up (+Z).
		outSide = { glm::vec3(0.0f, centre.y, centre.z), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(half.y, half.z) };
		// Top: the horizontal card, seen from +Z (above); u along the branch (+Y). Its up is -X so that
		// cross(normal, tangent) = up, the bitangent the lit FS rebuilds.
		outTop = { glm::vec3(centre.x, centre.y, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(half.y, half.x) };
	}

	TreeBillboardView billboardHorizontalView(const TreeBillboardBox& box)
	{
		const float halfY = (box.max.y - box.min.y) * 0.5f;
		const float maxX = glm::max(-box.min.x, box.max.x), maxZ = glm::max(-box.min.z, box.max.z);
		// Centred ON the axis (x = z = 0) at mid height, seen from +Y (above). u (right) along +Z, v (up) along +X:
		// cross(normal, right) = up and cross(right, up) = normal, the frames the lit FS rebuilds. u spans at least
		// the tree's height, so half its length - the lit FS's crown radius - matches the vertical cards'.
		return { glm::vec3(0.0f, (box.min.y + box.max.y) * 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(glm::max(halfY, maxZ), maxX) };
	}

	TreeBillboardLayout billboardLayout(uint32 size, uint32 numViews, bool horizontal)
	{
		TreeBillboardLayout layout;
		layout.numStrips = (horizontal ? 3u : 2u) * (numViews >= 4 ? 2u : 1u);
		// The strip height a multiple of the largest power of two p that leaves >= 4 px per strip at mip log2(p): every
		// strip boundary then falls on a texel boundary at every mip the material gets, so no mip mixes two cards.
		// 512 / 3 strips: p = 32, 160 rows each (32 spare rows at the bottom).
		const uint32 raw = size / layout.numStrips;
		uint32 p = 1;
		while (raw / (p * 2) >= 4)
			p *= 2;
		layout.stripHeight = glm::max(raw / p, 1u) * p;
		layout.numMips = 1;
		for (uint32 s = p; s > 1; s /= 2)
			++layout.numMips;
		return layout;
	}

	// The cards in strip order: side, top, then (horizontal) the horizontal card.
	static uint32 billboardCards(const TreeBillboardBox& box, bool horizontal, TreeBillboardView (&views)[3])
	{
		billboardViews(box, views[0], views[1]);
		if (!horizontal)
			return 2;
		views[2] = billboardHorizontalView(box);
		return 3;
	}

	void billboardMesh(const TreeBillboardBox& box, uint32 size, uint32 numViews, bool horizontal, TreeMesh& out, bool axisInZ)
	{
		const bool backViews = numViews >= 4;
		TreeBillboardView views[3];
		const uint32 numCards = billboardCards(box, horizontal, views);
		const float stripV = (float)billboardLayout(size, numViews, horizontal).stripHeight / (float)size; // one strip in v
		for (uint32 v = 0; v < numCards; ++v)
		{
			const TreeBillboardView& view = views[v];
			const glm::vec3 r = view.right * view.halfExtent.x;
			const glm::vec3 u = view.up * view.halfExtent.y;
			const glm::vec3 corners[4] = { view.centre - r - u, view.centre + r - u, view.centre + r + u, view.centre - r + u };
			// Image u along right; image v down from the card's top, into this face's strip of the texture.
			const glm::vec2 uvs[4] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
			for (int face = 0; face < 2; ++face)
			{
				const uint32 base = out.numVertices();
				// 4 views: a strip per face. 2 views: a strip per card, the back face reading the front's strip
				// through the card (the same u along `right`).
				const float strip = backViews ? (float)(v * 2 + (uint32)face) : (float)v;
				// The piece's axis (along `right` = +Y, through the origin) on this card: image v = 0.5 - its offset along
				// `up` from the centre / the card height (v runs down from the top), into this face's strip.
				const float axisV = (0.5f + glm::dot(view.centre, view.up) / (2.0f * view.halfExtent.y) + strip) * stripV;
				const float axisZ = axisInZ && v < 2 ? TREE_CARD_AXIS_CODE + axisV : 0.0f;
				for (int k = 0; k < 4; ++k)
				{
					out.positions.push_back(corners[k]);
					out.normals.push_back(face == 0 ? view.normal : -view.normal);
					out.tangents.push_back(view.right);
					// Same tangent and bitangent on the back face: its handedness flips, which is exactly the TBN
					// its own view was baked in (right / up kept, direction negated - see bakeBillboards).
					out.bitangents.push_back(view.up);
					out.texCoords.push_back(glm::vec3(uvs[k].x, (uvs[k].y + strip) * stripV, axisZ));
					out.bones.push_back(0);
				}
				if (face == 0)
					out.indices.insert(out.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
				else
					out.indices.insert(out.indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
			}
		}
	}

	void bakeBillboards(const TreePiece& piece, const TreeBakeImage& bark, const TreeBakeImage& leaves, uint32 size, float normalBend,
		uint32 numViews, bool horizontal, oc::vector<uint8>& outAlbedo, oc::vector<uint8>& outNormal)
	{
		const uint32 facesPerCard = numViews >= 4 ? 2u : 1u;
		outAlbedo.assign((size_t)size * size * 4, 0);
		outNormal.assign((size_t)size * size * 4, 0);
		// The spare rows below the last strip keep a neutral normal at depth 0.
		for (size_t t = 0; t < (size_t)size * size; ++t)
		{
			outNormal[t * 4 + 0] = 128;
			outNormal[t * 4 + 1] = 128;
			outNormal[t * 4 + 2] = 255;
			outNormal[t * 4 + 3] = 128;
		}
		const TreeBillboardBox box = billboardBox(piece);
		const glm::vec3 volumeCentre = (box.min + box.max) * 0.5f; // the clump's centre (the cards sit on the axis)
		TreeBillboardView views[3];
		const uint32 numCards = billboardCards(box, horizontal, views);
		const uint32 stripHeight = billboardLayout(size, numViews, horizontal).stripHeight;
		FrameTarget target;
		for (uint32 v = 0; v < numCards; ++v)
		{
			const TreeBillboardView& bv = views[v];
			for (uint32 face = 0; face < facesPerCard; ++face)
			{
				// The back face's view looks from the other side (direction negated) but keeps the card's
				// right / up: the image is stored as seen THROUGH the card, so the back face - which samples it with
				// the same u along `right` - shows it the right way round, and its baked tangent-space normals match
				// the back face's TBN (same tangent and bitangent, normal negated).
				const ViewBasis view{ bv.centre, bv.right, bv.up, face == 0 ? bv.normal : -bv.normal, bv.halfExtent };
				target.clear(size * SUPERSAMPLE, stripHeight * SUPERSAMPLE);
				rasterMesh(target, piece.bark[0], view, bark, false);
				rasterMesh(target, piece.leaves[0], view, leaves, true);
				// Depth (normal alpha) in units of the card's full u length: the lit FS rebuilds that length from the
				// screen derivatives of position and u (instance scale included) and moves the shadow lookup there.
				resolveView(target, view, 2.0f * bv.halfExtent.x, outAlbedo, outNormal, size, 0,
					(v * facesPerCard + face) * stripHeight, &volumeCentre, normalBend);
				// Every strip's depth is signed along the card's FRONT normal, the axis both faces rebuild from
				// their derivatives: a back view's depth (toward its own viewer) is negated.
				if (face == 1)
					for (size_t t = (size_t)(v * facesPerCard + face) * stripHeight * size; t < (size_t)(v * facesPerCard + face + 1) * stripHeight * size; ++t)
						outNormal[t * 4 + 3] = (uint8)(255 - outNormal[t * 4 + 3]);
			}
		}
	}
}

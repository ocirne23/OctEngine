module Spatial;

import Core;
import Core.glm;
import Core.Tweaks;

void SpatialOccluder::reset()
{
    if (m_handle.isValid())
    {
        Globals::occlusionBuffer.removeOccluder(m_handle);
        m_handle = {};
    }
}

void OcclusionBuffer::initialize()
{
    m_depth.resize(Width * Height);
    m_blockMax.resize(BlocksX * BlocksY);
    Tweak::boolean("Spatial/Occlusion", "Enabled", &m_enabled);
    Tweak::floatVar("Spatial/Occlusion", "Depth bias", &m_depthBias, 0.0f, 0.05f, 0.0001f);
    Tweak::intVar("Spatial/Occlusion", "Max occluder tris", &m_maxTriangles, 64, 16384);
    Tweak::intVar("Spatial/Occlusion", "Triangles", &m_statTriangles, 0, INT32_MAX);
    Tweak::intVar("Spatial/Occlusion", "Hidden cells", &m_statHiddenCells, 0, INT32_MAX);
    Tweak::floatVar("Spatial/Occlusion", "Raster ms", &m_statRasterMs, 0.0f, FLT_MAX, 0.001f);
}

oc::shared_ptr<const OccluderData> OcclusionBuffer::extractOccluders(oc::span<const glm::vec3> vertices,
                                                                      oc::span<const uint32> indices, uint32 maxTriangles)
{
    struct TriArea
    {
        float area;
        uint32 tri;
    };
    const uint32 numTris = uint32(indices.size() / 3);
    oc::vector<TriArea> areas;
    areas.reserve(numTris);
    for (uint32 t = 0; t < numTris; ++t)
    {
        const glm::vec3& a = vertices[indices[t * 3 + 0]];
        const glm::vec3& b = vertices[indices[t * 3 + 1]];
        const glm::vec3& c = vertices[indices[t * 3 + 2]];
        const float area = 0.5f * glm::length(glm::cross(b - a, c - a));
        if (area > 1e-6f)
            areas.push_back({ area, t });
    }
    if (areas.empty())
        return nullptr;
    const uint32 keep = glm::min(uint32(areas.size()), maxTriangles);
    oc::nth_element(areas.begin(), areas.begin() + keep - 1, areas.end(),
        [](const TriArea& a, const TriArea& b) { return a.area > b.area; });

    auto data = oc::make_shared<OccluderData>();
    data->vertices.reserve(size_t(keep) * 3);
    for (uint32 i = 0; i < keep; ++i)
    {
        const uint32 t = areas[i].tri;
        data->vertices.push_back(vertices[indices[t * 3 + 0]]);
        data->vertices.push_back(vertices[indices[t * 3 + 1]]);
        data->vertices.push_back(vertices[indices[t * 3 + 2]]);
    }
    return data;
}

SpatialHandle OcclusionBuffer::addOccluder(const oc::shared_ptr<const OccluderData>& data, const Transform& world)
{
    if (!data || data->vertices.empty())
        return {};
    uint32 idx;
    if (!m_freeOccluders.empty())
    {
        idx = m_freeOccluders.back();
        m_freeOccluders.pop_back();
    }
    else
    {
        idx = uint32(m_occluders.size());
        m_occluders.emplace_back();
    }
    Occluder& occluder = m_occluders[idx];
    occluder.used = true;
    occluder.worldVertices.resize(data->vertices.size());
    for (size_t i = 0; i < data->vertices.size(); ++i)
        occluder.worldVertices[i] = world.transformPoint(data->vertices[i]);
    return { idx, occluder.gen };
}

void OcclusionBuffer::removeOccluder(SpatialHandle handle)
{
    if (handle.idx >= m_occluders.size() || m_occluders[handle.idx].gen != handle.gen || !m_occluders[handle.idx].used)
        return;
    Occluder& occluder = m_occluders[handle.idx];
    occluder.used = false;
    occluder.worldVertices.clear();
    occluder.worldVertices.shrink_to_fit();
    ++occluder.gen;
    m_freeOccluders.push_back(handle.idx);
}

void OcclusionBuffer::render(const glm::mat4& viewProjRelCamera, const glm::dvec3& cameraPos)
{
    m_rendered = false;
    m_statHiddenCells = 0;
    if (!m_enabled)
        return;
    const auto start = Clock::now();
    m_viewProjRel = viewProjRelCamera;
    m_depth.assign(Width * Height, 1e30f);
    m_statTriangles = 0;

    for (const Occluder& occluder : m_occluders)
    {
        if (!occluder.used)
            continue;
        const size_t numVerts = occluder.worldVertices.size();
        for (size_t i = 0; i + 2 < numVerts; i += 3)
        {
            glm::vec4 clip[3];
            for (uint32 k = 0; k < 3; ++k)
            {
                const glm::vec3 rel = glm::vec3(glm::dvec3(occluder.worldVertices[i + k]) - cameraPos);
                clip[k] = m_viewProjRel * glm::vec4(rel, 1.0f);
            }
            clipAndRasterize(clip);
        }
    }

    for (uint32 by = 0; by < BlocksY; ++by)
    {
        for (uint32 bx = 0; bx < BlocksX; ++bx)
        {
            float blockMax = 0.0f;
            const float* row = &m_depth[by * BlockSize * Width + bx * BlockSize];
            for (uint32 y = 0; y < BlockSize; ++y, row += Width)
                for (uint32 x = 0; x < BlockSize; ++x)
                    blockMax = glm::max(blockMax, row[x]);
            m_blockMax[by * BlocksX + bx] = blockMax;
        }
    }

    m_statRasterMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    m_rendered = true;
}

void OcclusionBuffer::clipAndRasterize(const glm::vec4 (&clip)[3])
{
    // Sutherland-Hodgman against w > NearEpsilon; a triangle crossing the near plane yields up to
    // four vertices, fanned back into triangles.
    glm::vec4 out[4];
    uint32 numOut = 0;
    for (uint32 k = 0; k < 3; ++k)
    {
        const glm::vec4& current = clip[k];
        const glm::vec4& previous = clip[(k + 2) % 3];
        const bool currentIn = current.w > NearEpsilon;
        const bool previousIn = previous.w > NearEpsilon;
        if (currentIn != previousIn)
        {
            const float t = (NearEpsilon - previous.w) / (current.w - previous.w);
            out[numOut++] = previous + (current - previous) * t;
        }
        if (currentIn)
            out[numOut++] = current;
    }
    if (numOut < 3)
        return;
    rasterizeTriangle(out[0], out[1], out[2]);
    if (numOut == 4)
        rasterizeTriangle(out[0], out[2], out[3]);
}

void OcclusionBuffer::rasterizeTriangle(const glm::vec4& c0, const glm::vec4& c1, const glm::vec4& c2)
{
    const auto toScreen = [](const glm::vec4& c)
    {
        const float invW = 1.0f / c.w;
        return glm::vec3((c.x * invW * 0.5f + 0.5f) * float(Width), (c.y * invW * 0.5f + 0.5f) * float(Height), c.z * invW);
    };
    glm::vec3 s0 = toScreen(c0), s1 = toScreen(c1), s2 = toScreen(c2);

    float area = (s1.x - s0.x) * (s2.y - s0.y) - (s1.y - s0.y) * (s2.x - s0.x);
    if (area < 0.0f) // rasterize both windings: occluders are solid, min-depth keeps the near face
    {
        oc::swap(s1, s2);
        area = -area;
    }
    if (area < 1e-4f)
        return;
    ++m_statTriangles;

    const int minX = glm::max(int(glm::floor(glm::min(s0.x, glm::min(s1.x, s2.x)))), 0);
    const int maxX = glm::min(int(glm::ceil(glm::max(s0.x, glm::max(s1.x, s2.x)))), int(Width) - 1);
    const int minY = glm::max(int(glm::floor(glm::min(s0.y, glm::min(s1.y, s2.y)))), 0);
    const int maxY = glm::min(int(glm::ceil(glm::max(s0.y, glm::max(s1.y, s2.y)))), int(Height) - 1);
    if (minX > maxX || minY > maxY)
        return;

    // Edge functions at pixel centers, stepped incrementally across the row/column.
    const auto edge = [](const glm::vec3& a, const glm::vec3& b, float px, float py)
    {
        return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
    };
    const float startX = float(minX) + 0.5f;
    const float startY = float(minY) + 0.5f;
    float row0 = edge(s1, s2, startX, startY);
    float row1 = edge(s2, s0, startX, startY);
    float row2 = edge(s0, s1, startX, startY);
    const float dx0 = -(s2.y - s1.y), dy0 = s2.x - s1.x;
    const float dx1 = -(s0.y - s2.y), dy1 = s0.x - s2.x;
    const float dx2 = -(s1.y - s0.y), dy2 = s1.x - s0.x;
    const float invArea = 1.0f / area;

    for (int y = minY; y <= maxY; ++y)
    {
        float e0 = row0, e1 = row1, e2 = row2;
        float* depthRow = &m_depth[size_t(y) * Width];
        for (int x = minX; x <= maxX; ++x)
        {
            if (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f)
            {
                const float z = (e0 * s0.z + e1 * s1.z + e2 * s2.z) * invArea; // z/w is affine in screen space
                depthRow[x] = glm::min(depthRow[x], z);
            }
            e0 += dx0; e1 += dx1; e2 += dx2;
        }
        row0 += dy0; row1 += dy1; row2 += dy2;
    }
}

bool OcclusionBuffer::isVisible(const glm::vec3& centerRelCamera, const glm::vec3& halfExtent)
{
    if (!m_rendered)
        return true;

    glm::vec2 screenMin(1e30f), screenMax(-1e30f);
    float minZ = 1e30f;
    for (uint32 corner = 0; corner < 8; ++corner)
    {
        const glm::vec3 p = centerRelCamera + glm::vec3(
            (corner & 1) ? halfExtent.x : -halfExtent.x,
            (corner & 2) ? halfExtent.y : -halfExtent.y,
            (corner & 4) ? halfExtent.z : -halfExtent.z);
        const glm::vec4 clip = m_viewProjRel * glm::vec4(p, 1.0f);
        if (clip.w < NearEpsilon)
            return true; // crosses the near plane: never occluded
        const float invW = 1.0f / clip.w;
        const float sx = (clip.x * invW * 0.5f + 0.5f) * float(Width);
        const float sy = (clip.y * invW * 0.5f + 0.5f) * float(Height);
        screenMin = glm::min(screenMin, glm::vec2(sx, sy));
        screenMax = glm::max(screenMax, glm::vec2(sx, sy));
        minZ = glm::min(minZ, clip.z * invW);
    }
    if (screenMax.x < 0.0f || screenMin.x >= float(Width) || screenMax.y < 0.0f || screenMin.y >= float(Height))
        return true; // fully off-screen rects are the frustum test's business

    minZ -= m_depthBias;
    const int bx0 = glm::max(int(screenMin.x) / int(BlockSize), 0);
    const int bx1 = glm::min(int(screenMax.x) / int(BlockSize), int(BlocksX) - 1);
    const int by0 = glm::max(int(screenMin.y) / int(BlockSize), 0);
    const int by1 = glm::min(int(screenMax.y) / int(BlockSize), int(BlocksY) - 1);
    for (int by = by0; by <= by1; ++by)
        for (int bx = bx0; bx <= bx1; ++bx)
            if (m_blockMax[by * BlocksX + bx] >= minZ)
                return true; // some pixel in this block is not covered nearer than the cell
    ++m_statHiddenCells;
    return false;
}

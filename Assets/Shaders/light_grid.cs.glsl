#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
//#extension GL_EXT_debug_printf : enable

#include "shared.inc.glsl"

struct LightInfo
{
	vec3 pos;
	float range;     // negative = tube light 
	vec3 color;
	float width;     // negative = spotlight angle, 0 = point light, > 0 = area/tube light width
	vec3 direction;  // magnitude = height
	float rotation;
};
layout (binding = 1, std430) readonly buffer InLightInfos
{
	LightInfo in_lightInfos[];
};
layout (binding = 2, std430) coherent buffer OutLightGrids
{
    uint inout_gridData[];
};
layout (binding = 3, std430) coherent buffer InOutTable
{
    uint inout_numGrids;
    uint inout_gridDataCounter;
    uint inout_tableSize;
    uint inout_gridTable[];
};

#define GRID_DATA_WRITE
#define GRID_DATA_NAME         inout_gridData
#define NUM_GRIDS_NAME         inout_numGrids
#define GRID_DATA_COUNTER_NAME inout_gridDataCounter
#define TABLE_SIZE_NAME        inout_tableSize
#define GRID_TABLE_NAME        inout_gridTable
#include "light_grid.inc.glsl"

// Distance LOD (injected from LightGridParams; the fallbacks reproduce the engine defaults):
//   level    = floor(pow(max(dist - START, 0) / STEP, POWER))
//   cellSize = clamp(MIN_CELL << level, MIN_CELL, MAX_CELL)   [world units per cell]
// MIN_CELL 1 = GRID_SIZE cells per axis (full resolution); MAX_CELL GRID_SIZE = one cell per grid.
#ifndef LIGHT_GRID_LOD_START
#define LIGHT_GRID_LOD_START 0.0
#endif
#ifndef LIGHT_GRID_LOD_STEP
#define LIGHT_GRID_LOD_STEP 16.0
#endif
#ifndef LIGHT_GRID_LOD_POWER
#define LIGHT_GRID_LOD_POWER 0.5
#endif
// The engine picks the curve's cheap form from the exponent: 1 = sqrt (power 0.5), 2 = linear
// (power 1), 0 = the general pow.
#ifndef LIGHT_GRID_LOD_CURVE
#define LIGHT_GRID_LOD_CURVE 1
#endif
#ifndef LIGHT_GRID_MIN_CELL
#define LIGHT_GRID_MIN_CELL 1u
#endif
#ifndef LIGHT_GRID_MAX_CELL
#define LIGHT_GRID_MAX_CELL uint(GRID_SIZE)
#endif
// A light spanning more cells than this in one grid is added as that grid's LARGE light (one
// entry, evaluated by every pixel of the grid) instead of per cell: a wide light in a full-res
// grid would otherwise cost this ONE thread tens of thousands of atomics.
#ifndef LIGHT_GRID_CELL_BUDGET
#define LIGHT_GRID_CELL_BUDGET 1024
#endif

uint lodCellSize(ivec3 gridPos)
{
    const float viewDist = distance(vec3(gridPos) * GRID_SIZE + GRID_SIZE / 2, u_viewPos);
    const float t = max(viewDist - LIGHT_GRID_LOD_START, 0.0) * (1.0 / LIGHT_GRID_LOD_STEP);
#if LIGHT_GRID_LOD_CURVE == 1
    const float level = sqrt(t);
#elif LIGHT_GRID_LOD_CURVE == 2
    const float level = t;
#else
    const float level = pow(t, LIGHT_GRID_LOD_POWER);
#endif
    const uint cellSize = LIGHT_GRID_MIN_CELL << uint(min(level, 8.0));
    return clamp(cellSize, LIGHT_GRID_MIN_CELL, LIGHT_GRID_MAX_CELL);
}

// ONE lane per workgroup ON PURPOSE: getOrInsertGrid spins on a slot another thread marked
// INITIALIZING_ENTRY. Lanes of one wave have no forward-progress guarantee against each other, so
// a wider workgroup can deadlock a wave on its own insert. Do not widen without fixing that.
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;

void main()
{
    const uint lightIdx   = gl_GlobalInvocationID.x;
    const LightInfo light = in_lightInfos[lightIdx];

    float reach = abs(light.range);
    vec3 lightMin = light.pos - vec3(reach);
    vec3 lightMax = light.pos + vec3(reach);
    float sphereRadius = reach; // point / spot: cells outside the range sphere are skipped
    if (light.width > 0.0 && light.range < 0.0)
    {
        sphereRadius = 0.0; // capsule: keep the box
        // Tube light: capsule along the axis. Bound as the two end-cap spheres of radius + absRange.
        const float height   = length(light.direction);
        const float halfLen  = height * 0.5;
        const vec3  axis     = light.direction / height;
        const float absRange = -light.range;
        const float radius   = light.width;
        const float pad      = radius + absRange;
        reach = halfLen + pad; // conservative radius for large-light threshold
        const vec3 pa = light.pos - axis * halfLen;
        const vec3 pb = light.pos + axis * halfLen;
        lightMin = min(pa, pb) - vec3(pad);
        lightMax = max(pa, pb) + vec3(pad);
    }
    else if (light.width > 0.0)
    {
        sphereRadius = 0.0; // quad: keep the box
        // Area light: bound the front-facing influence box. The quad only emits along +normal, so
        // the box spans [0, range] on the normal axis (dropping the always-culled back half) and
        // half-extent + range on the in-plane axes. Build the same right/up/normal frame as shading.
        const float height = length(light.direction);
        const vec3 up = light.direction / height;
        const vec3 ref = abs(up.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        const vec3 right0 = normalize(cross(up, ref));
        const vec3 right = right0 * cos(light.rotation) + cross(up, right0) * sin(light.rotation);
        const vec3 normal = cross(up, right);
        reach += 0.5 * sqrt(light.width * light.width + height * height); // keep the per-cell/large-light threshold conservative
        const float heRight  = light.width * 0.5 + light.range;
        const float heUp     = height * 0.5 + light.range;
        const vec3 boxCenter = light.pos + normal * (light.range * 0.5);
        const vec3 extent = heRight * abs(right) + heUp * abs(up) + (light.range * 0.5) * abs(normal);
        lightMin = boxCenter - extent;
        lightMax = boxCenter + extent;
    }
    else if (light.width < 0.0)
    {
        // Spot light: tighten to the cone's spherical sector (apex + base disk + axial cap tip),
        // which is far smaller than the full range sphere for narrow cones. rotation is the
        // half-angle (<= 90 deg); at 90 deg this collapses to the front-hemisphere box.
        const vec3 axis = normalize(light.direction);
        const vec3 tip  = light.pos + axis * light.range;
        const vec3 c    = light.pos + axis * (light.range * cos(light.rotation));
        const vec3 e    = (light.range * sin(light.rotation)) * sqrt(max(vec3(1.0) - axis * axis, vec3(0.0)));
        lightMin = min(light.pos, min(tip, c - e));
        lightMax = max(light.pos, max(tip, c + e));
    }

    const ivec3 gridMin = getGridPos(lightMin);
    const ivec3 gridMax = getGridPos(lightMax);

    for (int x = gridMin.x; x <= gridMax.x; ++x)
    {
        for (int y = gridMin.y; y <= gridMax.y; ++y)
        {
            for (int z = gridMin.z; z <= gridMax.z; ++z)
            {
                // Depends only on the grid position, so every light that touches a grid agrees
                // on its cell size (getOrInsertGrid keeps the first one).
                const uint cellSize = lodCellSize(ivec3(x, y, z));
                const uint gridIdx = getOrInsertGrid(ivec3(x, y, z), cellSize);
                if (gridIdx == INVALID_GRID) // out of table/data space; buffers grow next frame
                    continue;
                // The cells this light's box spans INSIDE this grid, at this grid's resolution.
                const ivec3 gridMinW = ivec3(x, y, z) * GRID_SIZE;
                const ivec3 minCell = max(ivec3(floor(lightMin)) - gridMinW, ivec3(0)) / int(cellSize);
                const ivec3 maxCell = min(ivec3(floor(lightMax)) - gridMinW, ivec3(GRID_SIZE - 1)) / int(cellSize);
                const ivec3 span = maxCell - minCell + 1;
                if (reach > float(GRID_SIZE / 2) || span.x * span.y * span.z > LIGHT_GRID_CELL_BUDGET)
                {
                    addLargeLight(gridIdx, lightIdx);
                }
                else
                {
                    addLightToGrid(gridIdx, lightIdx, minCell, maxCell, light.pos, sphereRadius);
                }
            }
        }
    }
}
// THE FACTION MAP (Renderer::setFactionMap): the game's faction layer as one R8 texture over a world-FIXED square -
// 0 = fully the enemy's, 128 = neutral (the world as generated), 255 = fully the players'. The ground's look follows it.
// Requires FACTION_MAP_BINDING (24 in the forward set, 20 in the far-tree march's), the UBO, and - without
// FACTION_NO_HALF - GL_EXT_shader_explicit_arithmetic_types.

#ifndef TERRAIN_FACTION_INC_GLSL
#define TERRAIN_FACTION_INC_GLSL

layout (binding = FACTION_MAP_BINDING) uniform sampler2D u_factionMap;

// -1 = fully the enemy's .. 0 = neutral .. 1 = fully the players'. 0 with no map; outside its square the map's START
// value (u_terrain_factionOutside), so the square's edge does not show.
// THE NIGHT (u_terrain_factionNight, 0 .. 1, Renderer::setFactionNight): land UNDER neutral goes to the full enemy side
// by it - untouched land starts one byte under neutral (no tint by day), so by night all of it has the enemy's look.
float terrainFactionSide(vec2 xz)
{
	if (u_terrain_factionInvSize <= 0.0)
		return 0.0;
	const vec2 uv = (xz - u_terrain_factionOrigin) * u_terrain_factionInvSize;
	float side = u_terrain_factionOutside;
	// No clamp: texel 0 gives -1.008, and every reader takes min(|side| x gain, 1) or a fog factor that small step does not show in.
	if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))))
		side = textureLod(u_factionMap, uv, 0.0).r * (255.0 / 127.0) - (128.0 / 127.0);
	return side < 0.0 ? side - u_terrain_factionNight * (1.0 + side) : side;
}

// The ground's albedo by the side ("Game/Faction"): the surface keeps its LIGHT AND DARK (its luminance) and takes the
// side's picked colour as its hue - the same result on grass, rock and sand. A pale picked colour keeps more of the
// surface's own colour, and white at brightness 1 is no tint. The CPU folds the colour, its brightness and its
// saturation into two colours a side (keep / add); here it is one dot and two multiply-adds. The weight is the side x
// "Tint gain" (2 = the full tint from half-held land on). scale =the surface's share of the tint: 1 for what grows (the ground, grass, leaves, flowers),
// u_terrain_factionSolidScale ("Trunk and rock tint") for bark and rock.
vec3 terrainFactionTintSide(vec3 albedo, float side, float scale)
{
	if (side == 0.0)
		return albedo;
	// out = albedo x (1 + w x keep) + luminance x (w x add): one dot and two multiply-adds. keep / add are the side's two
	// CPU-made colours (RendererUbo.cpp factionTint): the surface's own colour goes down by the tint's saturation while
	// its LUMINANCE x the picked colour comes in - so every surface, whatever its own colour, takes the same hue.
	const float w = min(abs(side) * u_terrain_factionGain, 1.0) * scale;
	const bool enemy = side < 0.0;
	const float luminance = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
	return min(albedo * (1.0 + w * (enemy ? u_terrain_factionEnemyKeep : u_terrain_factionPlayerKeep))
		+ (luminance * w) * (enemy ? u_terrain_factionEnemyAdd : u_terrain_factionPlayerAdd), vec3(1.0));
}
vec3 terrainFactionTint(vec3 albedo, vec2 xz, float scale) { return terrainFactionTintSide(albedo, terrainFactionSide(xz), scale); }
// THE FOG on a side's land ("Game/Faction" "Fog tint", "Enemy land fog"): its albedo takes the tint by the fog's own
// share, and enemy land holds MORE fog - "Enemy land fog" x the enemy side, ADDED to the regional fog factor (the
// climate's fog thickness, 0 in a dry region): a multiplier on it gave enemy land fog only where the climate already had some.
vec3 terrainFactionFogAlbedo(vec3 albedo, float side) { return terrainFactionTintSide(albedo, side, u_terrain_factionFogTint); }
float terrainFactionFogExtra(float side) { return u_terrain_factionEnemyFog * max(-side, 0.0); }
// The half forms (the lit shaders' albedo): the same rule IN HALF - the only fp32 -> fp16 conversion is the weight (the
// keep / add colours are baked constants while their rows are locked). An fp32 albedo takes terrainFactionTint instead.
// FACTION_NO_HALF: an includer with no 16-bit types (the far-tree march).
#ifndef FACTION_NO_HALF
f16vec3 terrainFactionAlbedo(f16vec3 albedoH, vec2 xz, float scale)
{
	const float side = terrainFactionSide(xz);
	if (side == 0.0)
		return albedoH;
	const float16_t w = float16_t(min(abs(side) * u_terrain_factionGain, 1.0) * scale);
	const bool enemy = side < 0.0;
	const float16_t luminance = dot(albedoH, f16vec3(0.2126, 0.7152, 0.0722));
	return min(albedoH * (float16_t(1.0) + w * f16vec3(enemy ? u_terrain_factionEnemyKeep : u_terrain_factionPlayerKeep))
		+ (luminance * w) * f16vec3(enemy ? u_terrain_factionEnemyAdd : u_terrain_factionPlayerAdd), f16vec3(1.0));
}
f16vec3 terrainFactionAlbedo(f16vec3 albedoH, vec2 xz) { return terrainFactionAlbedo(albedoH, xz, 1.0); }
#endif

#endif

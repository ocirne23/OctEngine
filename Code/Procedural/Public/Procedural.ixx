export module Procedural;

export import :Noise;
export import :TerrainSampler;
// The terrain generator (Private/Diffusion) is the only public part of it; every other Diffusion partition
// stays internal so ONNX Runtime and FastNoiseLite never reach this surface.
export import :GeneratorV3;
export import :TerrainChunk;
export import :TerrainGenerator;
export import :HeightMapBaker;
export import :TerrainStreamer;
export import :TerrainPreview;
export import :TerrainCollider;
export import :OceanGenerator;
export import :TreeSpecies;
export import :TreeGenerator;
export import :TreeLeafTexture;
export import :TreeBarkTexture;
export import :TreeImpostor;
export import :TreeWorld;
export import :TreeSystem;
export import :RockType;
export import :RockGenerator;
export import :RockSystem;
export import :ClutterType;
export import :ClutterGenerator;
export import :ClutterSystem;

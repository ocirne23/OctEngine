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
export import :Scattering;
export import :OceanGenerator;

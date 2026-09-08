export module RendererVK:LightGridComputePipeline;

import Core;

import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;
import :Settings;

export class LightGridComputePipeline final
{
public:

	// `params` (the distance LOD) is baked into the shader as #defines: reloadShaders() re-reads it.
	void initialize(const LightGridParams* params);
	void reloadShaders();
	struct RecordParams
	{
		DescriptorSet& descriptorSet;
		Buffer& ubo;
		Buffer& inLightInfoBuffer;
		Buffer& outLightGridBuffer;
		Buffer& outLightTableBuffer;
		uint32 numTableEntries; // hash table size written into the table header (power of 2)
	};
	void record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& recordParams);
	void update(uint32 frameIdx, uint32 numLights);
	vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_computePipeline.getDescriptorSetLayout(); }

private:
	struct PerFrameData
	{
		Buffer inIndirectCommandBuffer;
		oc::span<vk::DispatchIndirectCommand> mappedIndirectCommands;
	};
	void buildComputeLayout(ComputePipelineLayout& layout);

	oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
	ComputePipeline m_computePipeline;
	const LightGridParams* m_params = nullptr;
};
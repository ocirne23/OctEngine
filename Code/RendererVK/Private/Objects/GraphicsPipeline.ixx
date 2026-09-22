export module RendererVK:GraphicsPipeline;

import Core;
import :VK;
import :Layout;
import :Shader;

class RenderPass;

export struct VertexLayoutInfo
{
    oc::vector<vk::VertexInputBindingDescription> bindingDescriptions;
    oc::vector<vk::VertexInputAttributeDescription> attributeDescriptions;
};

export struct ShaderSource
{
    oc::string text;
    oc::string debugFilePath;
    oc::vector<ShaderDefine> defines;
};

export struct PipelineVariant
{
    // Per-variant shader overrides; an empty text field falls back to the layout default. The tess ones only
    // where the layout itself has tessellation (the terrain film displaces to its own water level).
    ShaderSource vertexShader;
    ShaderSource fragmentShader;
    ShaderSource tessEvalShader;
    // Per-variant pipeline state. Transparent variants enable alpha blending and disable depth
    // writes; opaque variants (the default) keep blending off and depth writes on.
    bool blendEnable = false;
    // DUAL-SOURCE composite instead of the "over" blend (with blendEnable): out = src0 + dst * src1, per
    // channel - the fragment shader writes location 0 index 0 (added colour) and index 1 (dst multiplier).
    // The dst alpha is kept, as for every blended variant, unless dualSourceAlpha.
    bool dualSourceBlend = false;
    // With dualSourceBlend: the ALPHA composites the same way (out.a = src0.a + dst.a * src1.a) instead of
    // being kept. For a surface that owns the scene colour's alpha (TAA's ocean flag) where it is opaque.
    bool dualSourceAlpha = false;
    bool depthWrite = true;
    // Gizmo/overlay variants disable the depth test so they draw on top of everything regardless
    // of scene depth; everything else keeps the layout's depthTestEnable.
    bool depthTest = true;
    // Depth test EQUAL instead of the layout's compare: a variant that re-draws a surface already in the depth
    // buffer (the terrain overlay) - its VS must produce bit-identical depth (`invariant gl_Position`).
    bool depthEqual = false;
    // Wireframe variants set eLine; everything else keeps the solid fill of variant 0.
    vk::PolygonMode polygonMode = vk::PolygonMode::eFill;
    // Per-variant face culling. Defaults to the back-face culling variant 0 uses; set eFront or eNone
    // for inverted / double-sided variants (e.g. wireframe and gizmo overlays).
    vk::CullModeFlags cullMode = vk::CullModeFlagBits::eBack;
};

export struct GraphicsPipelineLayout
{
    ShaderSource vertexShader;
    ShaderSource fragmentShader;
    // Optional TESSELLATION, for EVERY variant (one module each, so the variants' positions are bit-identical -
    // what an EQUAL-depth re-draw needs): both set = patch-list topology with patchControlPoints. The domain
    // origin is LOWER_LEFT, so the evaluation shader's winding keywords follow the GL rules.
    ShaderSource tessControlShader;
    ShaderSource tessEvalShader;
    uint32 patchControlPoints = 3;
    // INDIRECT_BINDABLE (DGC execution sets). Off for a pipeline drawn only by plain commands - a tess pipeline
    // cannot join an execution set whose initial pipeline has other stages anyway.
    bool indirectBindable = true;
    VertexLayoutInfo vertexLayoutInfo;
    oc::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
    // Optional, parallel to descriptorSetLayoutBindings. If non-empty, the set layout is created
    // UPDATE_AFTER_BIND-capable and each binding gets the corresponding flags (e.g. eUpdateAfterBind for
    // a descriptor that's refreshed after the command buffer is recorded, like a per-frame TLAS).
    oc::vector<vk::DescriptorBindingFlags> descriptorBindingFlags;
    oc::vector<vk::PushConstantRange> pushConstantRanges;

    // Depth-only passes (e.g. shadow maps) bind no fragment shader and write to a render pass with
    // no color attachments. depthBias is slope-scaled to fight shadow acne, and a configurable cull
    // mode lets shadow passes cull front faces to reduce peter-panning.
    bool depthOnly = false;
    bool depthBiasEnable = false;
    float depthBiasConstantFactor = 0.0f;
    float depthBiasSlopeFactor = 0.0f;
    vk::CullModeFlags cullMode = vk::CullModeFlagBits::eBack;

    // Variant-0 polygon mode (additional variants carry their own); the global wireframe toggle sets eLine.
    vk::PolygonMode polygonMode = vk::PolygonMode::eFill;

    // Primitive topology for every variant (debug line passes set eLineList).
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;

    // Variant-0 blend/depth state, for fullscreen overlay passes (e.g. the volumetric fog apply, which
    // blends src.rgb + dst.rgb * src.a over the lit scene with depth ignored).
    bool blendEnable = false;
    vk::BlendFactor srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    vk::BlendFactor dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    // eMin/eMax ignore the factors (the force-shell interval pass MIN-blends its ray interval).
    vk::BlendOp colorBlendOp = vk::BlendOp::eAdd;
    // false = RGB only, every variant. The scene colour's ALPHA is TAA's animated-surface flag (the
    // ocean writes 0 - see taa.cs.glsl), so the stages that layer over the opaque scene (decals,
    // debug draws, force, particles, fog apply) must leave it alone.
    bool colorWriteAlpha = true;
    bool depthTestEnable = true;
    bool depthWriteEnable = true;
    // The main view renders REVERSED-Z (near = 1, far = 0, cleared to 0) for far-field depth precision,
    // so closer = greater. Shadow maps keep standard-Z ortho projections and override this with eLess.
    vk::CompareOp depthCompareOp = vk::CompareOp::eGreater;

    // Additional pipeline variants for DGC Indirect Execution Set selection. Variant 0 always
    // uses both layout defaults; each extra entry can independently override either shader stage.
    oc::vector<PipelineVariant> additionalVariants;
};

export class GraphicsPipeline final
{
public:
    GraphicsPipeline();
    ~GraphicsPipeline();
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    bool initialize(const RenderPass& renderPass, GraphicsPipelineLayout& layout);
    bool reloadShaders(const RenderPass& renderPass, GraphicsPipelineLayout& layout);
    // Overloads for passes that own a raw vk::RenderPass (e.g. the depth-only shadow pass).
    bool initialize(vk::RenderPass renderPass, GraphicsPipelineLayout& layout);
    bool reloadShaders(vk::RenderPass renderPass, GraphicsPipelineLayout& layout);

    vk::Pipeline getPipeline() const { return m_pipelines[0]; }
    vk::Pipeline getPipelineVariant(uint32 variantIdx) const { return m_pipelines[variantIdx]; }
    uint32 getPipelineVariantCount() const { return (uint32)m_pipelines.size(); }
    vk::PipelineLayout getPipelineLayout() const { return m_pipelineLayout; }
    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }

private:

    bool createPipelines(vk::RenderPass renderPass, GraphicsPipelineLayout& layout, oc::vector<vk::Pipeline>& outPipelines, bool assertOnFailure);

    oc::vector<vk::Pipeline> m_pipelines;
    vk::PipelineCache m_pipelineCache;
    vk::PipelineLayout m_pipelineLayout;
    vk::DescriptorSetLayout m_descriptorSetLayout;
};
module File;

import Core;

import :SceneData;
import :ProceduralSceneData;

oc::unique_ptr<ISceneData> ISceneData::createAssimpLoader()
{
	return oc::make_unique<SceneData>();
}

oc::unique_ptr<ISceneData> ISceneData::createProceduralLoader()
{
	return oc::make_unique<ProceduralSceneData>();
}

oc::unique_ptr<ISceneData> ISceneData::createMeshScene(const MeshGeometryDesc& geometry,
	const uint8* colorRGBA, uint32 colorWidth, uint32 colorHeight)
{
	auto scene = oc::make_unique<ProceduralSceneData>();
	if (!scene->initializeFromMesh(geometry, colorRGBA, colorWidth, colorHeight))
		return nullptr;
	return scene;
}

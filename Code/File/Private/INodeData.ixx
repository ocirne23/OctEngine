export module File:INodeData;

import File.fwd;

import Core;
import Core.glm;

export class INodeData
{
public:
    virtual ~INodeData() = default;

    virtual operator bool() const = 0;
    virtual bool operator==(const INodeData& other) const = 0;
    virtual bool isValid() const = 0;

    virtual oc::unique_ptr<INodeData> clone() const = 0;

    virtual const char* getName() const = 0;

    virtual uint32 getNumChildren() const = 0;
    virtual oc::unique_ptr<INodeData> getChild(uint32 idx) const = 0;

    virtual uint32 getNumChildrenRecursive() const = 0;

    //virtual oc::unique_ptr<INodeData> findChild(oc::initializer_list<const char*> hierarchy) const = 0;

    virtual oc::vector<oc::string> getChildrenNames() const = 0;

    virtual uint32 getNumMeshes() const = 0;
    virtual uint32 getMeshIndex(uint32 meshIdx) const = 0;

    virtual void getTransform(glm::vec3& pos, glm::vec3& scale, glm::quat& rot) const = 0;
    virtual glm::vec3 getPosition() const = 0;
    virtual glm::vec3 getScale() const = 0;
    virtual glm::quat getRotation() const = 0;
};
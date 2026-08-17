export module File:AssetParser;

import File.fwd;

import Core;
import Core.glm;

// Grammar (indentation defines hierarchy, tabs or spaces):
//   Key value value ...        -> key + values
//   "quoted value"             -> a single value, may contain spaces
//   0, 0, 0                    -> commas are treated as separators (vectors)
//   # ... or // ...            -> comment line
export struct AssetNode
{
    oc::string key;
    oc::vector<oc::string> values;
    oc::vector<AssetNode> children;

    bool hasValue(size_t idx = 0) const { return idx < values.size(); }
    size_t numValues() const { return values.size(); }

    const oc::string& asString(size_t idx = 0) const
    {
        static const oc::string empty;
        return idx < values.size() ? values[idx] : empty;
    }
    bool asBool(size_t idx = 0, bool fallback = false) const;
    int asInt(size_t idx = 0, int fallback = 0) const;
    float asFloat(size_t idx = 0, float fallback = 0.0f) const;
    glm::vec3 asVec3(const glm::vec3& fallback = glm::vec3(0.0f)) const;

    // Case-insensitive lookup of a direct child by key (keys are human-authored).
    const AssetNode* find(oc::string_view childKey) const;
    oc::vector<const AssetNode*> findAll(oc::string_view childKey) const;

    AssetNode& addChild(oc::string key);
    AssetNode& set(oc::string key, oc::string value);          // single string value
    AssetNode& set(oc::string key, const char* str);            // single string value
    AssetNode& set(oc::string key, float value);
    AssetNode& set(oc::string key, bool value);
    AssetNode& set(oc::string key, const glm::vec3& value);     // "x, y, z"
};

export bool parseAssetText(oc::string_view text, AssetNode& outRoot, oc::string& outError);
export bool loadAssetFile(const oc::string& path, AssetNode& outRoot, oc::string& outError);
export oc::string writeAssetText(const AssetNode& root);

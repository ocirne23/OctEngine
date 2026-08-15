module File;

import Core;
import Core.glm;

import :AssetParser;
import :FileSystem;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

static bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

// Splits a line into whitespace/comma-separated tokens, keeping "quoted strings" intact.
static std::vector<std::string> tokenizeLine(std::string_view line)
{
    std::vector<std::string> tokens;
    const size_t n = line.size();
    size_t i = 0;
    while (i < n)
    {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == ',' || c == '\r')
        {
            ++i;
            continue;
        }
        if (c == '"')
        {
            ++i;
            std::string tok;
            while (i < n && line[i] != '"')
                tok.push_back(line[i++]);
            if (i < n)
                ++i; // closing quote
            tokens.push_back(std::move(tok));
        }
        else
        {
            const size_t start = i;
            while (i < n)
            {
                const char d = line[i];
                if (d == ' ' || d == '\t' || d == ',' || d == '\r' || d == '"')
                    break;
                ++i;
            }
            tokens.emplace_back(line.substr(start, i - start));
        }
    }
    return tokens;
}

bool AssetNode::asBool(size_t idx, bool fallback) const
{
    if (idx >= values.size())
        return fallback;
    const std::string& v = values[idx];
    if (iequals(v, "true") || iequals(v, "1") || iequals(v, "yes"))
        return true;
    if (iequals(v, "false") || iequals(v, "0") || iequals(v, "no"))
        return false;
    return fallback;
}

int AssetNode::asInt(size_t idx, int fallback) const
{
    if (idx >= values.size())
        return fallback;
    const std::string& v = values[idx];
    int result = fallback;
    std::from_chars(v.data(), v.data() + v.size(), result);
    return result;
}

float AssetNode::asFloat(size_t idx, float fallback) const
{
    if (idx >= values.size())
        return fallback;
    const std::string& v = values[idx];
    float result = fallback;
    std::from_chars(v.data(), v.data() + v.size(), result);
    return result;
}

glm::vec3 AssetNode::asVec3(const glm::vec3& fallback) const
{
    return glm::vec3(asFloat(0, fallback.x), asFloat(1, fallback.y), asFloat(2, fallback.z));
}

const AssetNode* AssetNode::find(std::string_view childKey) const
{
    for (const AssetNode& child : children)
        if (iequals(child.key, childKey))
            return &child;
    return nullptr;
}

std::vector<const AssetNode*> AssetNode::findAll(std::string_view childKey) const
{
    std::vector<const AssetNode*> result;
    for (const AssetNode& child : children)
        if (iequals(child.key, childKey))
            result.push_back(&child);
    return result;
}

AssetNode& AssetNode::addChild(std::string a_key)
{
    AssetNode node;
    node.key = std::move(a_key);
    children.push_back(std::move(node));
    return children.back();
}

AssetNode& AssetNode::set(std::string a_key, const char* value)
{
    AssetNode& node = addChild(std::move(a_key));
    node.values.push_back(value);
    return node;
}

AssetNode& AssetNode::set(std::string a_key, std::string value)
{
    AssetNode& node = addChild(std::move(a_key));
    node.values.push_back(std::move(value));
    return node;
}

AssetNode& AssetNode::set(std::string a_key, float value)
{
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    return set(std::move(a_key), std::string(buf, ptr));
}

AssetNode& AssetNode::set(std::string a_key, bool value)
{
    return set(std::move(a_key), std::string(value ? "true" : "false"));
}

AssetNode& AssetNode::set(std::string a_key, const glm::vec3& value)
{
    AssetNode& node = addChild(std::move(a_key));
    node.values.resize(3);
    for (int i = 0; i < 3; ++i)
    {
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value[i]);
        node.values[i].assign(buf, ptr);
    }
    return node;
}

bool parseAssetText(std::string_view text, AssetNode& outRoot, std::string& outError)
{
    outRoot = AssetNode{};
    outError.clear();

    std::vector<size_t> path;  // indices from root to the current node
    std::vector<int> indents;  // indent width of the node at each path level

    const size_t len = text.size();
    size_t pos = 0;
    while (pos < len)
    {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = len;
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;

        // Measure indentation and locate the first meaningful character.
        int indent = 0;
        size_t firstNonWs = 0;
        while (firstNonWs < line.size() && (line[firstNonWs] == ' ' || line[firstNonWs] == '\t'))
        {
            ++indent;
            ++firstNonWs;
        }

        if (firstNonWs >= line.size() || line[firstNonWs] == '\r')
            continue; // blank line
        if (line[firstNonWs] == '#' ||
            (line[firstNonWs] == '/' && firstNonWs + 1 < line.size() && line[firstNonWs + 1] == '/'))
            continue; // comment

        std::vector<std::string> tokens = tokenizeLine(line);
        if (tokens.empty())
            continue;

        AssetNode node;
        node.key = std::move(tokens[0]);
        node.values.reserve(tokens.size() - 1);
        for (size_t i = 1; i < tokens.size(); ++i)
            node.values.push_back(std::move(tokens[i]));

        // Pop back up to this line's parent (the nearest ancestor with a smaller indent).
        while (!indents.empty() && indent <= indents.back())
        {
            indents.pop_back();
            path.pop_back();
        }

        // Re-resolve the parent each line so growing child vectors never leave stale pointers.
        AssetNode* parent = &outRoot;
        for (size_t idx : path)
            parent = &parent->children[idx];

        parent->children.push_back(std::move(node));
        path.push_back(parent->children.size() - 1);
        indents.push_back(indent);
    }
    return true;
}

bool loadAssetFile(const std::string& path, AssetNode& outRoot, std::string& outError)
{
    // Asset TEXT loads (.pre/.oc/.anm/.apl) are main-thread by design: World::buildTemplate reads a
    // prefab the first time it is spawned and caches the template, so this is a warm-up cost, not a
    // per-frame one. Marked explicitly rather than silently tripping the FileSystem main-thread assert.
    const std::string content = FileSystem::readFileStr(path, /*allowMainThread*/ true);
    if (content.empty())
    {
        outError = "Could not read file (missing or empty): " + path;
        return false;
    }
    return parseAssetText(content, outRoot, outError);
}

// A value needs quoting if it is empty or contains a separator/quote character.
static bool needsQuoting(const std::string& v)
{
    if (v.empty())
        return true;
    for (char c : v)
        if (c == ' ' || c == '\t' || c == ',' || c == '"' || c == '#')
            return true;
    return false;
}

static void writeNode(const AssetNode& node, int depth, std::string& out)
{
    out.append(size_t(depth), '\t');
    out += node.key;
    for (const std::string& v : node.values)
    {
        out += ' ';
        if (needsQuoting(v))
        {
            out += '"';
            out += v;
            out += '"';
        }
        else
        {
            out += v;
        }
    }
    out += '\n';
    for (const AssetNode& child : node.children)
        writeNode(child, depth + 1, out);
}

std::string writeAssetText(const AssetNode& root)
{
    std::string out;
    for (const AssetNode& child : root.children)
        writeNode(child, 0, out);
    return out;
}

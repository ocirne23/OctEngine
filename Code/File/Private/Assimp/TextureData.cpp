module;

#include "AssimpGmf.h"

module File;

import Core;

import :TextureData;
import :Assimp;

using namespace Assimp;

TextureData::TextureData()
{
}

TextureData::~TextureData()
{
}

// Loose file on disk: store path only; pixel data is loaded later by the consumer.
bool TextureData::initialize(const char* filePath)
{
    m_filePath = filePath ? filePath : "";
    m_pTexture = nullptr;
    return !m_filePath.empty();
}

// Embedded texture baked into the scene file.
bool TextureData::initialize(const aiTexture* pTexture)
{
    m_pTexture = pTexture;
    m_filePath = m_pTexture->mFilename.C_Str();
    return true;
}

const char* TextureData::getFileName() const
{
    return m_filePath.c_str();
}

// Returns nullptr for loose file textures; the consumer should load from getFileName().
const TextureData::Pixel* TextureData::getPixels() const
{
    if (!m_pTexture)
        return nullptr;
    return reinterpret_cast<const Pixel*>(m_pTexture->pcData);
}

uint32 TextureData::getWidth() const
{
    return m_pTexture ? m_pTexture->mWidth : 0;
}

uint32 TextureData::getHeight() const
{
    return m_pTexture ? m_pTexture->mHeight : 0;
}

const char* TextureData::getFormatInfo() const
{
    return m_pTexture ? m_pTexture->achFormatHint : "";
}
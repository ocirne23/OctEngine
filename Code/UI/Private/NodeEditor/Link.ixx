module;

#include "ImguiGmf.h"

export module UI:Link;

import :imgui_node_editor;

namespace NodeEditor
{

export class Link
{
public:

    void initialize(ed::PinId inputId, ed::PinId outputId);
    void update(double deltaSec, bool firstFrame);

    ed::LinkId getId() const { return ed::LinkId(this); }
    operator ed::LinkId() const { return ed::LinkId(this); }

    ed::PinId getInputId() const { return m_inputId; }
    ed::PinId getOutputId() const { return m_outputId; }

private:

    ed::PinId m_inputId;
    ed::PinId m_outputId;
};

} // namespace NodeEditor


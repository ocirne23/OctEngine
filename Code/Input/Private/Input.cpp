module Input;

import Core;
import Core.SDL;
import Core.Window;
import Core.imgui;

import UI;

import :Input;

bool Input::initialize()
{
    m_pKeyStates = SDL_GetKeyboardState(nullptr);
    return true;
}

bool Input::isKeyDownByName(const char* keyName) const
{
    return m_pKeyStates != nullptr && m_pKeyStates[scancodeFromName(keyName)];
}



namespace
{
oc::vector<SDL_Event> g_frameEvents; // this frame's pumped events (Input is a singleton)
} // namespace

void Input::update(double deltaSec)
{
    ProfileScope profileScope("Input", EProfileCategory::Input);
    ImGuiIO& imguiIO = ImGui::GetIO();

    // The window thread pumped while main blocked on the frame fence (requestPump at the loop
    // top); this waits out the rare case where it is still mid-pump, then drains the buffer.
    // Everything below (ImGui event processing, the listeners) runs on MAIN, exactly as before -
    // the window thread never touches the ImGui context or engine state.
    m_window->waitPumpDone();
    m_window->takeEvents(g_frameEvents);
    for (SDL_Event& evt : g_frameEvents)
    {
        ImGui_ImplSDL3_ProcessEvent(&evt);
        if (evt.type == SDL_EVENT_KEY_DOWN) // the Script Editor's raw-key path ignores everything else
            Globals::ui.handleKeyEvent(evt);

        // The DSL Script Editor handles its own raw key events (see ScriptEditor's class comment) rather than
        // through a normal ImGui widget, so it never sets WantCaptureKeyboard/WantTextInput on its own --
        // isScriptEditorFocused() is checked alongside those so typing a digit there (e.g. a vector literal's
        // "1.0,2.0,3.0") doesn't ALSO fire whatever global gameplay shortcut that key is bound to below.
        if (evt.type >= SDL_EVENT_KEY_DOWN && evt.type <= SDL_EVENT_TEXT_INPUT
            && (imguiIO.WantCaptureKeyboard || imguiIO.WantTextInput || Globals::ui.isScriptEditorFocused())
            && !Globals::ui.isViewportFocused())
            continue;
        if (!(evt.type == SDL_EVENT_MOUSE_BUTTON_UP && evt.button.button == 1 && Globals::ui.isViewportGrabbed())) // pass through mouse button up when viewport is grabbed to prevent stuck mouse
            if (evt.type >= SDL_EVENT_MOUSE_MOTION && evt.type <= SDL_EVENT_MOUSE_WHEEL && (imguiIO.WantCaptureMouse && (!Globals::ui.isViewportFocused() || Globals::ui.isViewportGrabbed())))
                continue;

        if (evt.type >= SDL_EVENT_WINDOW_FIRST && evt.type <= SDL_EVENT_WINDOW_LAST)
        {
            for (auto& listener : m_systemEventListeners)
                if (listener->onWindowEvent) listener->onWindowEvent(evt.window);
        }

        switch (evt.type)
        {
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            m_windowHasFocus = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_windowHasFocus = false;
            break;
        case SDL_EVENT_QUIT:
            for (auto& listener : m_systemEventListeners)
                if (listener->onQuit) listener->onQuit();
            break;
        case SDL_EVENT_MOUSE_MOTION:
            for (auto& listener : m_mouseListeners)
                if (listener->onMouseMoved) listener->onMouseMoved(evt.motion);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            for (auto& listener : m_mouseListeners)
                if (listener->onMouseWheelMoved) listener->onMouseWheelMoved(evt.wheel);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            ProfileScope scope("Mouse button dispatch", EProfileCategory::Input);
            for (auto& listener : m_mouseListeners)
                if (listener->onMousePressed) listener->onMousePressed(evt.button);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            ProfileScope scope("Mouse button dispatch", EProfileCategory::Input);
            for (auto& listener : m_mouseListeners)
                if (listener->onMouseReleased) listener->onMouseReleased(evt.button);
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            if (evt.key.repeat == 0)
            {
                // The heavy one-shot handlers live here: F5 shader reload, F6 script recompile,
                // the spawn/possess keys - a fat "Input" frame is almost always one of these.
                ProfileScope scope("Key dispatch", EProfileCategory::Input);
                for (auto& listener : m_keyboardListeners)
                    if (listener->onKeyPressed) listener->onKeyPressed(evt.key);
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (evt.key.repeat == 0)
            {
                ProfileScope scope("Key dispatch", EProfileCategory::Input);
                for (auto& listener : m_keyboardListeners)
                    if (listener->onKeyReleased) listener->onKeyReleased(evt.key);
            }
            break;
        default:
            break;
        }
    }
}
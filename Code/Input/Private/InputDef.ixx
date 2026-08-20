export module Input:Input;

import Core;
import Core.fwd; // Window (the event-source pointer below)

export struct SDL_MouseWheelEvent;
export struct SDL_MouseMotionEvent;
export struct SDL_MouseButtonEvent;

export struct SDL_TextEditingEvent;
export struct SDL_TextInputEvent;
export struct SDL_KeyboardEvent;

export struct SDL_JoyButtonEvent;
export struct SDL_JoyAxisEvent;
export struct SDL_JoyHatEvent;

export struct SDL_WindowEvent;

export enum SDL_Scancode : int;

export class MouseListener
{
public:
    oc::function<void(const SDL_MouseMotionEvent&)> onMouseMoved;
    oc::function<void(const SDL_MouseWheelEvent&)>  onMouseWheelMoved;
    oc::function<void(const SDL_MouseButtonEvent&)> onMousePressed;
    oc::function<void(const SDL_MouseButtonEvent&)> onMouseReleased;
};

export class KeyboardListener
{
public:
    oc::function<void(const SDL_TextEditingEvent&)> onTextEditing;
    oc::function<void(const SDL_TextInputEvent&)>   onTextInput;
    oc::function<void(const SDL_KeyboardEvent&)>    onKeyPressed;
    oc::function<void(const SDL_KeyboardEvent&)>    onKeyReleased;
};

export class SystemEventListener
{
public:
    oc::function<void(const SDL_WindowEvent&)> onWindowEvent;
    oc::function<void()> onQuit;
};

export class JoystickListener
{
public:
    virtual void joyButtonPressed(const SDL_JoyButtonEvent& evt, int button) {}
    virtual void joyButtonReleased(const SDL_JoyButtonEvent& evt, int button) {}
    virtual void joyAxisMoved(const SDL_JoyAxisEvent& arg, int axis) {}
    virtual void joyPovMoved(const SDL_JoyHatEvent& arg, int index) {}
};

// RAII handle to a registered listener: Input::add*Listener returns one, the destructor unregisters.
// Move-only; assign the callback slots through -> (the listener object itself is owned by Input and
// address-stable). Must not outlive Globals::input (a static-teardown holder needs init_seg ordering).
export template<typename T>
class InputListenerHandle final
{
public:
    InputListenerHandle() = default;
    ~InputListenerHandle() { reset(); }
    InputListenerHandle(InputListenerHandle&& other) noexcept : m_listener(oc::exchange(other.m_listener, nullptr)) {}
    InputListenerHandle& operator=(InputListenerHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_listener = oc::exchange(other.m_listener, nullptr);
        }
        return *this;
    }
    InputListenerHandle(const InputListenerHandle&) = delete;
    InputListenerHandle& operator=(const InputListenerHandle&) = delete;

    T* operator->() const { return m_listener; }
    explicit operator bool() const { return m_listener != nullptr; }
    void reset(); // unregisters now; defined below Globals::input

private:
    friend class Input;
    explicit InputListenerHandle(T* listener) : m_listener(listener) {}
    T* m_listener = nullptr;
};

export using MouseListenerHandle       = InputListenerHandle<MouseListener>;
export using KeyboardListenerHandle    = InputListenerHandle<KeyboardListener>;
export using SystemEventListenerHandle = InputListenerHandle<SystemEventListener>;

export class Input final
{
public:

    Input() = default;
    ~Input() = default;
    Input(const Input&) = delete;
    Input(const Input&&) = delete;
    Input& operator=(const Input&) = delete;
    Input& operator=(const Input&&) = delete;

    bool initialize();

    // The window thread owns the OS pump (see Core.Window); update() drains its event buffer.
    void setEventSource(Window* window) { m_window = window; }
    void update(double deltaSec);

    void setMouseInWindow(bool inWindow)       { m_mouseInWindow = inWindow; }
    void setWindowHasFocus(bool hasFocus)      { m_windowHasFocus = hasFocus; }
    void setWantMouseGrab(bool wantGrab)       { m_wantMousieGrab = wantGrab; }
    void setWantMouseVisible(bool wantVisible) { m_wantMouseVisible = wantVisible; }
    void setMouseCaptured(bool captured)       { m_mouseCaptured = captured; } // a tool (e.g. gizmo) owns the left-drag

    bool isKeyDown(SDL_Scancode key) const { return m_pKeyStates[key]; }
    bool isKeyDownByName(const char* keyName) const; // resolves the name via Core.SDL scancodeFromName
    bool isMouseInWindow() const           { return m_mouseInWindow; }
    bool isWindowHasFocus() const          { return m_windowHasFocus; }
    bool wantMouseGrab() const             { return m_wantMousieGrab; }
    bool wantMouseVisible() const          { return m_wantMouseVisible; }
    bool isMouseCaptured() const           { return m_mouseCaptured; }

    [[nodiscard]] MouseListenerHandle addMouseListener()             { return MouseListenerHandle(m_mouseListeners.emplace_back(oc::make_unique<MouseListener>()).get()); }
    [[nodiscard]] KeyboardListenerHandle addKeyboardListener()       { return KeyboardListenerHandle(m_keyboardListeners.emplace_back(oc::make_unique<KeyboardListener>()).get()); }
    [[nodiscard]] SystemEventListenerHandle addSystemEventListener() { return SystemEventListenerHandle(m_systemEventListeners.emplace_back(oc::make_unique<SystemEventListener>()).get()); }

private:

    template<typename T> friend class InputListenerHandle;
    void removeListener(const MouseListener* listener)       { oc::erase_if(m_mouseListeners,       [listener](const auto& l) { return l.get() == listener; }); }
    void removeListener(const KeyboardListener* listener)    { oc::erase_if(m_keyboardListeners,    [listener](const auto& l) { return l.get() == listener; }); }
    void removeListener(const SystemEventListener* listener) { oc::erase_if(m_systemEventListeners, [listener](const auto& l) { return l.get() == listener; }); }

    const bool* m_pKeyStates = nullptr;
    Window* m_window = nullptr; // event source: the window thread's pumped buffer (see Core.Window)
    oc::vector<oc::unique_ptr<MouseListener>>    m_mouseListeners;
    oc::vector<oc::unique_ptr<KeyboardListener>> m_keyboardListeners;
    oc::vector<oc::unique_ptr<SystemEventListener>> m_systemEventListeners;

    bool m_mouseCaptured = false;
    bool m_mouseInWindow = false;
    bool m_windowHasFocus = false;
    bool m_wantMousieGrab = false;
    bool m_wantMouseVisible = true;
};

export namespace Globals
{
    Input input;
}

export template<typename T>
void InputListenerHandle<T>::reset()
{
    if (m_listener)
    {
        Globals::input.removeListener(m_listener);
        m_listener = nullptr;
    }
}
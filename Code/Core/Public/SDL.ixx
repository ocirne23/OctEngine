module;

#ifndef _MSC_VER
#include "LinuxGmfPrelude.h"
#endif

export module Core.SDL;

import Core;

#ifdef _MSC_VER
export import <SDL3/SDL.h>;
export import <SDL3/SDL_vulkan.h>;
export import <SDL3/SDL_scancode.h>;
#else
export extern "C++"
{
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_scancode.h>
}
#endif

#undef SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER
#undef SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_MAXIMIZED_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_MENU_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_MINIMIZED_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_MODAL_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_MOUSE_GRABBED_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_PARENT_POINTER
#undef SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_TITLE_STRING
#undef SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_TOOLTIP_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_UTILITY_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER
#undef SDL_PROP_WINDOW_CREATE_X_NUMBER
#undef SDL_PROP_WINDOW_CREATE_Y_NUMBER
#undef SDL_PROP_WINDOW_CREATE_COCOA_WINDOW_POINTER
#undef SDL_PROP_WINDOW_CREATE_COCOA_VIEW_POINTER
#undef SDL_PROP_WINDOW_CREATE_WAYLAND_SCALE_TO_DISPLAY_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_WAYLAND_CREATE_EGL_WINDOW_BOOLEAN
#undef SDL_PROP_WINDOW_CREATE_WAYLAND_WL_SURFACE_POINTER
#undef SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER
#undef SDL_PROP_WINDOW_CREATE_WIN32_PIXEL_FORMAT_HWND_POINTER
#undef SDL_PROP_WINDOW_CREATE_X11_WINDOW_NUMBER

export const char* SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN               = "always_on_top"				  ;
export const char* SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN                  = "borderless"				  ;
export const char* SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN                   = "focusable"					  ;
export const char* SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN   = "external_graphics_context"	  ;
export const char* SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN                  = "fullscreen"				  ;
export const char* SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER                       = "height"					  ;
export const char* SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN                      = "hidden"					  ;
export const char* SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN          = "high_pixel_density"		  ;
export const char* SDL_PROP_WINDOW_CREATE_MAXIMIZED_BOOLEAN                   = "maximized"					  ;
export const char* SDL_PROP_WINDOW_CREATE_MENU_BOOLEAN                        = "menu"						  ;
export const char* SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN                       = "metal"						  ;
export const char* SDL_PROP_WINDOW_CREATE_MINIMIZED_BOOLEAN                   = "minimized"					  ;
export const char* SDL_PROP_WINDOW_CREATE_MODAL_BOOLEAN                       = "modal"						  ;
export const char* SDL_PROP_WINDOW_CREATE_MOUSE_GRABBED_BOOLEAN               = "mouse_grabbed"				  ;
export const char* SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN                      = "opengl"					  ;
export const char* SDL_PROP_WINDOW_CREATE_PARENT_POINTER                      = "parent"					  ;
export const char* SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN                   = "resizable"					  ;
export const char* SDL_PROP_WINDOW_CREATE_TITLE_STRING                        = "title"						  ;
export const char* SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN                 = "transparent"				  ;
export const char* SDL_PROP_WINDOW_CREATE_TOOLTIP_BOOLEAN                     = "tooltip"					  ;
export const char* SDL_PROP_WINDOW_CREATE_UTILITY_BOOLEAN                     = "utility"					  ;
export const char* SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN                      = "vulkan"					  ;
export const char* SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER                        = "width"						  ;
export const char* SDL_PROP_WINDOW_CREATE_X_NUMBER                            = "x"							  ;
export const char* SDL_PROP_WINDOW_CREATE_Y_NUMBER                            = "y"							  ;
export const char* SDL_PROP_WINDOW_CREATE_COCOA_WINDOW_POINTER                = "cocoa.window"				  ;
export const char* SDL_PROP_WINDOW_CREATE_COCOA_VIEW_POINTER                  = "cocoa.view"				  ;
export const char* SDL_PROP_WINDOW_CREATE_WAYLAND_SCALE_TO_DISPLAY_BOOLEAN    = "wayland.scale_to_display"	  ;
export const char* SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN = "wayland.surface_role_custom" ;
export const char* SDL_PROP_WINDOW_CREATE_WAYLAND_CREATE_EGL_WINDOW_BOOLEAN   = "wayland.create_egl_window"	  ;
export const char* SDL_PROP_WINDOW_CREATE_WAYLAND_WL_SURFACE_POINTER          = "wayland.wl_surface"		  ;
export const char* SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER                  = "win32.hwnd"				  ;
export const char* SDL_PROP_WINDOW_CREATE_WIN32_PIXEL_FORMAT_HWND_POINTER     = "win32.pixel_format_hwnd"	  ;
export const char* SDL_PROP_WINDOW_CREATE_X11_WINDOW_NUMBER                   = "x11.window"				  ;

// SDL_KMOD_CTRL is a plain #define (SDL_KMOD_LCTRL | SDL_KMOD_RCTRL); like the props above, #define's don't
// cross the module boundary, so it's re-exported here as a real typed constant for checking SDL_KeyboardEvent::mod.
#undef SDL_KMOD_CTRL
export constexpr uint16 SDL_KMOD_CTRL = 0x0040u | 0x0080u; // SDL_KMOD_LCTRL | SDL_KMOD_RCTRL
#undef SDL_KMOD_SHIFT
export constexpr uint16 SDL_KMOD_SHIFT = 0x0001u | 0x0002u; // SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT

#undef SDL_WINDOW_FULLSCREEN
#undef SDL_WINDOW_OPENGL              
#undef SDL_WINDOW_OCCLUDED            
#undef SDL_WINDOW_HIDDEN              
#undef SDL_WINDOW_BORDERLESS          
#undef SDL_WINDOW_RESIZABLE           
#undef SDL_WINDOW_MINIMIZED           
#undef SDL_WINDOW_MAXIMIZED           
#undef SDL_WINDOW_MOUSE_GRABBED       
#undef SDL_WINDOW_INPUT_FOCUS         
#undef SDL_WINDOW_MOUSE_FOCUS         
#undef SDL_WINDOW_EXTERNAL            
#undef SDL_WINDOW_MODAL               
#undef SDL_WINDOW_HIGH_PIXEL_DENSITY  
#undef SDL_WINDOW_MOUSE_CAPTURE       
#undef SDL_WINDOW_ALWAYS_ON_TOP       
#undef SDL_WINDOW_UTILITY             
#undef SDL_WINDOW_TOOLTIP             
#undef SDL_WINDOW_POPUP_MENU          
#undef SDL_WINDOW_KEYBOARD_GRABBED    
#undef SDL_WINDOW_VULKAN              
#undef SDL_WINDOW_METAL               
#undef SDL_WINDOW_TRANSPARENT         
#undef SDL_WINDOW_NOT_FOCUSABLE       

export constexpr uint32 SDL_WINDOW_FULLSCREEN          = 0x00000001U; /**< window is in fullscreen mode */
export constexpr uint32 SDL_WINDOW_OPENGL              = 0x00000002U; /**< window usable with OpenGL context */
export constexpr uint32 SDL_WINDOW_OCCLUDED            = 0x00000004U; /**< window is occluded */
export constexpr uint32 SDL_WINDOW_HIDDEN              = 0x00000008U; /**< window is neither mapped onto the desktop nor shown in the taskbar/dock/window list; SDL_ShowWindow() is required for it to become visible */
export constexpr uint32 SDL_WINDOW_BORDERLESS          = 0x00000010U; /**< no window decoration */
export constexpr uint32 SDL_WINDOW_RESIZABLE           = 0x00000020U; /**< window can be resized */
export constexpr uint32 SDL_WINDOW_MINIMIZED           = 0x00000040U; /**< window is minimized */
export constexpr uint32 SDL_WINDOW_MAXIMIZED           = 0x00000080U; /**< window is maximized */
export constexpr uint32 SDL_WINDOW_MOUSE_GRABBED       = 0x00000100U; /**< window has grabbed mouse input */
export constexpr uint32 SDL_WINDOW_INPUT_FOCUS         = 0x00000200U; /**< window has input focus */
export constexpr uint32 SDL_WINDOW_MOUSE_FOCUS         = 0x00000400U; /**< window has mouse focus */
export constexpr uint32 SDL_WINDOW_EXTERNAL            = 0x00000800U; /**< window not created by SDL */
export constexpr uint32 SDL_WINDOW_MODAL               = 0x00001000U; /**< window is modal */
export constexpr uint32 SDL_WINDOW_HIGH_PIXEL_DENSITY  = 0x00002000U; /**< window uses high pixel density back buffer if possible */
export constexpr uint32 SDL_WINDOW_MOUSE_CAPTURE       = 0x00004000U; /**< window has mouse captured (unrelated to MOUSE_GRABBED) */
export constexpr uint32 SDL_WINDOW_ALWAYS_ON_TOP       = 0x00008000U; /**< window should always be above others */
export constexpr uint32 SDL_WINDOW_UTILITY             = 0x00020000U; /**< window should be treated as a utility window, not showing in the task bar and window list */
export constexpr uint32 SDL_WINDOW_TOOLTIP             = 0x00040000U; /**< window should be treated as a tooltip and does not get mouse or keyboard focus, requires a parent window */
export constexpr uint32 SDL_WINDOW_POPUP_MENU          = 0x00080000U; /**< window should be treated as a popup menu, requires a parent window */
export constexpr uint32 SDL_WINDOW_KEYBOARD_GRABBED    = 0x00100000U; /**< window has grabbed keyboard input */
export constexpr uint32 SDL_WINDOW_VULKAN              = 0x10000000U; /**< window usable for Vulkan surface */
export constexpr uint32 SDL_WINDOW_METAL               = 0x20000000U; /**< window usable for Metal view */
export constexpr uint32 SDL_WINDOW_TRANSPARENT         = 0x40000000U; /**< window with transparent buffer */
export constexpr uint32 SDL_WINDOW_NOT_FOCUSABLE       = 0x80000000U; /**< window should not be focusable */

#undef SDL_INIT_AUDIO
#undef SDL_INIT_VIDEO
#undef SDL_INIT_JOYSTICK
#undef SDL_INIT_HAPTIC
#undef SDL_INIT_GAMEPAD
#undef SDL_INIT_EVENTS
#undef SDL_INIT_SENSOR
#undef SDL_INIT_CAMERA

export constexpr uint32 SDL_INIT_AUDIO      = 0x00000010u; /**< `SDL_INIT_AUDIO` implies `SDL_INIT_EVENTS` */
export constexpr uint32 SDL_INIT_VIDEO      = 0x00000020u; /**< `SDL_INIT_VIDEO` implies `SDL_INIT_EVENTS`, should be initialized on the main thread */
export constexpr uint32 SDL_INIT_JOYSTICK   = 0x00000200u; /**< `SDL_INIT_JOYSTICK` implies `SDL_INIT_EVENTS` */
export constexpr uint32 SDL_INIT_HAPTIC     = 0x00001000u;
export constexpr uint32 SDL_INIT_GAMEPAD    = 0x00002000u; /**< `SDL_INIT_GAMEPAD` implies `SDL_INIT_JOYSTICK` */
export constexpr uint32 SDL_INIT_EVENTS     = 0x00004000u;
export constexpr uint32 SDL_INIT_SENSOR     = 0x00008000u; /**< `SDL_INIT_SENSOR` implies `SDL_INIT_EVENTS` */
export constexpr uint32 SDL_INIT_CAMERA     = 0x00010000u; /**< `SDL_INIT_CAMERA` implies `SDL_INIT_EVENTS` */

// Maps a human-readable key name (e.g. "Space", "A", "Left Shift", "Escape") to its scancode value.
// Returns SDL_SCANCODE_UNKNOWN (0) if the name is empty or unrecognised. Names match SDL_GetScancodeName().
// Returns int rather than SDL_Scancode so it is callable from modules that forward-declare SDL_Scancode
// (e.g. Input) without the two enum declarations colliding across the module boundary.
export int scancodeFromName(const char* name)
{
    if (name == nullptr || name[0] == '\0')
        return SDL_SCANCODE_UNKNOWN;
    return SDL_GetScancodeFromName(name);
}
/*
  ==============================================================================

   This file is part of the JUCE 6 technical preview.
   Copyright (c) 2017 - ROLI Ltd.

   You may use this code under the terms of the GPL v3
   (see www.gnu.org/licenses).

   For this technical preview, this file is not subject to commercial licensing.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

#ifdef JUCE_GUI_EXTRA_H_INCLUDED
 /* When you add this cpp file to your project, you mustn't include it in a file where you've
    already included any other headers - just put it inside a file on its own, possibly with your config
    flags preceding it, but don't include anything else. That also includes avoiding any automatic prefix
    header files that the compiler may be using.
 */
 #error "Incorrect use of JUCE cpp file"
#endif

#define JUCE_CORE_INCLUDE_OBJC_HELPERS 1
#define JUCE_CORE_INCLUDE_COM_SMART_PTR 1
#define JUCE_CORE_INCLUDE_JNI_HELPERS 1
#define JUCE_CORE_INCLUDE_NATIVE_HEADERS 1
#define JUCE_EVENTS_INCLUDE_WIN32_MESSAGE_WINDOW 1
#define JUCE_GRAPHICS_INCLUDE_COREGRAPHICS_HELPERS 1

#if JUCE_USE_WINRT_WEBVIEW
 #define JUCE_EVENTS_INCLUDE_WINRT_WRAPPER 1
#endif

#ifndef JUCE_PUSH_NOTIFICATIONS
 #define JUCE_PUSH_NOTIFICATIONS 0
#endif

#include "juce_gui_extra.h"

//==============================================================================
#if JUCE_MAC
 #import <WebKit/WebKit.h>
 #import <IOKit/IOKitLib.h>
 #import <IOKit/IOCFPlugIn.h>
 #import <IOKit/hid/IOHIDLib.h>
 #import <IOKit/hid/IOHIDKeys.h>
 #import <IOKit/pwr_mgt/IOPMLib.h>

 #if JUCE_PUSH_NOTIFICATIONS
  #import <Foundation/NSUserNotification.h>

  #include "native/juce_mac_PushNotifications.cpp"
 #endif

//==============================================================================
#elif JUCE_IOS
 #import <WebKit/WebKit.h>

 #if JUCE_PUSH_NOTIFICATIONS
  #if defined (__IPHONE_10_0) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_10_0
   #import <UserNotifications/UserNotifications.h>
  #endif

  #include "native/juce_ios_PushNotifications.cpp"
 #endif

//==============================================================================
#elif JUCE_ANDROID
 #if JUCE_PUSH_NOTIFICATIONS
  #include "native/juce_android_PushNotifications.cpp"
 #endif

//==============================================================================
#elif JUCE_WINDOWS
 #include <windowsx.h>
 #include <vfw.h>
 #include <commdlg.h>

 #if JUCE_WEB_BROWSER
  #include <exdisp.h>
  #include <exdispid.h>
  #if JUCE_USE_WINRT_WEBVIEW
   #include <windows.web.ui.h>
   #include <windows.web.ui.interop.h>
   #include <windows.foundation.h>
   #include <windows.foundation.collections.h>

   #pragma warning (push)
   #pragma warning (disable: 4265)
   #include <wrl.h>
   #include <wrl/wrappers/corewrappers.h>
   #pragma warning (pop)
  #endif
 #endif

//==============================================================================
#elif JUCE_LINUX
 #include <X11/Xlib.h>
 #include <X11/Xatom.h>
 #include <X11/Xutil.h>
 #undef SIZEOF
 #undef KeyPress

 #if JUCE_WEB_BROWSER
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/wait.h>

  JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wzero-as-null-pointer-constant", "-Wparentheses")

  #include <gtk/gtk.h>

  JUCE_END_IGNORE_WARNINGS_GCC_LIKE

  #include <gtk/gtkx.h>
  #include <glib-unix.h>
  #include <webkit2/webkit2.h>
 #endif
#endif

//==============================================================================
#include "documents/juce_FileBasedDocument.cpp"
#include "code_editor/juce_CodeDocument.cpp"
#include "code_editor/juce_CodeEditorComponent.cpp"
#include "code_editor/juce_CPlusPlusCodeTokeniser.cpp"
#include "code_editor/juce_XMLCodeTokeniser.cpp"
#include "code_editor/juce_LuaCodeTokeniser.cpp"
#include "misc/juce_BubbleMessageComponent.cpp"
#include "misc/juce_ColourSelector.cpp"
#include "misc/juce_KeyMappingEditorComponent.cpp"
#include "misc/juce_PreferencesPanel.cpp"
#include "misc/juce_PushNotifications.cpp"
#include "misc/juce_RecentlyOpenedFilesList.cpp"
#include "misc/juce_SplashScreen.cpp"
#include "misc/juce_SystemTrayIconComponent.cpp"
#include "misc/juce_LiveConstantEditor.cpp"
#include "misc/juce_AnimatedAppComponent.cpp"

//==============================================================================
#if JUCE_MAC || JUCE_IOS

 JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wundeclared-selector")

 #if JUCE_MAC
  #include "native/juce_mac_NSViewComponent.mm"
  #include "native/juce_mac_AppleRemote.mm"
  #include "native/juce_mac_SystemTrayIcon.cpp"
 #endif

 #if JUCE_IOS
  #include "native/juce_ios_UIViewComponent.mm"
 #endif

 #if JUCE_WEB_BROWSER
  #include "native/juce_mac_WebBrowserComponent.mm"
 #endif

 JUCE_END_IGNORE_WARNINGS_GCC_LIKE

//==============================================================================
#elif JUCE_WINDOWS
 #include "native/juce_win32_ActiveXComponent.cpp"
 #if JUCE_WEB_BROWSER
  #include "native/juce_win32_WebBrowserComponent.cpp"
 #endif
 #include "native/juce_win32_SystemTrayIcon.cpp"

//==============================================================================
#elif JUCE_LINUX
 JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wzero-as-null-pointer-constant")

 #include "native/juce_linux_XEmbedComponent.cpp"

 #if JUCE_WEB_BROWSER
  #include "native/juce_linux_X11_WebBrowserComponent.cpp"
 #endif

 JUCE_END_IGNORE_WARNINGS_GCC_LIKE

 #include "native/juce_linux_X11_SystemTrayIcon.cpp"

//==============================================================================
#elif JUCE_ANDROID
 #include "native/juce_AndroidViewComponent.cpp"

 #if JUCE_WEB_BROWSER
  #include "native/juce_android_WebBrowserComponent.cpp"
 #endif
#endif

#if JUCE_WEB_BROWSER
namespace juce
{
    bool WebBrowserComponent::pageAboutToLoad (const String&)  { return true; }
    void WebBrowserComponent::pageFinishedLoading (const String&) {}
    bool WebBrowserComponent::pageLoadHadNetworkError (const String&) { return true; }
    void WebBrowserComponent::windowCloseRequest() {}
    void WebBrowserComponent::newWindowAttemptingToLoad (const String&) {}
}
#endif

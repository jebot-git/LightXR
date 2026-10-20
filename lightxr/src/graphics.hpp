#pragma once
#include <EGL/egl.h>

struct NativeGraphics {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config{};
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    NativeGraphics();
    ~NativeGraphics();
};

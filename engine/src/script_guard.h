// Cairn Engine - containment boundary around gameplay scripts (internal).
//
// Script code is compiled into a shared library loaded at runtime and runs in
// the same process as the editor or CLI. A C++ exception that crosses back out
// of the engine terminates the whole host application, so an erroring script
// takes the editor window down with it instead of being reported. Every engine
// call into script code goes through GuardScript, which logs the class and
// callback that failed and lets the frame continue.
#pragma once

#include "cairn/log.h"

#include <exception>
#include <string>

namespace Cairn {

template <typename Fn>
void GuardScript(const std::string& className, const char* method, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        Log::Error("script %s::%s threw: %s", className.c_str(), method, e.what());
    } catch (...) {
        Log::Error("script %s::%s threw an unknown exception", className.c_str(), method);
    }
}

}  // namespace Cairn

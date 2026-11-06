#!/usr/bin/env python
# Hewn — build script.
#   scons platform=windows target=editor            (the editor library: Hewn is an editor tool only;
#                                                    exported games get plain Godot nodes and need no library)
import os

env = SConscript("godot-cpp/SConstruct", {"api_version": "4.7"})

env.Append(CPPPATH=["src/"])
sources = Glob("src/*.cpp")
if env["target"] == "editor":
    sources += Glob("src/editor/*.cpp")

if env["platform"] != "windows" or env.get("use_mingw", False):
    env.Append(CXXFLAGS=["-std=c++17"])

libname = "hewn"
bindir = "project/addons/hewn/bin"
if env["platform"] == "macos":
    library = env.SharedLibrary(
        "{}/lib{}.{}.{}.framework/lib{}.{}.{}".format(bindir, libname, env["platform"], env["target"], libname, env["platform"], env["target"]),
        source=sources)
else:
    library = env.SharedLibrary(
        "{}/lib{}{}{}".format(bindir, libname, env["suffix"], env["SHLIBSUFFIX"]),
        source=sources)

Default(library)

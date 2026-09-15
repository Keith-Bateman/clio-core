cmake_minimum_required(VERSION 3.20)

#------------------------------------------------------------------------------
# DTProvenance agent-interceptor -- ported from clio-core-dtio-old
# (chimaera -> clio rename; see Phase A of the tool-call-cache plan).
#------------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)

include(FetchContent)

# cpp-httplib -- HTTP server (proxy) + HTTPS client (interception ChiMods).
# nlohmann_json is already vendored by the top-level CMakeLists.txt.
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.3
  GIT_SHALLOW    TRUE
)
set(HTTPLIB_COMPILE OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(httplib)

# Protocol library first -- standalone, no ChiMod dependency.
add_subdirectory(protocol)

# ChiMods in dependency order: ctx_untangler before tracker (tracker's client
# dispatches to it), tracker before the interception ChiMods and proxy
# (they call into the tracker client), proxy last.
add_subdirectory(ctx_untangler)
add_subdirectory(tracker)
add_subdirectory(intercept_anthropic)
add_subdirectory(intercept_openai)
add_subdirectory(intercept_ollama)
add_subdirectory(proxy)

# Demo launcher and verification tool
add_subdirectory(demo)
add_subdirectory(tools)

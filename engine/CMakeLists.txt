# Built inside BOSS's own source tree: the Dockerfile copies this folder to src/web-engine
# and adds it to src/CMakeLists.txt, so it links the same targets as BOSS's command-line slicer.
find_package(nlohmann_json REQUIRED)

add_executable(prusaslicer-web-engine src/main.cpp src/settings.cpp)
target_link_libraries(prusaslicer-web-engine PRIVATE
    libslic3r libcereal slic3r-arrange-wrapper libseqarrange stb_image
    nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS} Threads::Threads)

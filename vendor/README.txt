vendor/
=======

Nothing is required here. CMake downloads Dear ImGui and nlohmann/json on the
first configure. Use this folder only for offline builds or optional extras:

  vendor/imgui/                 Dear ImGui checkout (tag v1.92.2b-docking)
  vendor/nlohmann/json.hpp      nlohmann/json single header (v3.11.3)
  vendor/ffmpeg/ffmpeg.exe      Optional. Copied next to Clipstream.exe after each build.
                                Used for trimming/exporting clips and for test clips
                                while the capture engine is still a stub.
                                Windows builds: https://www.gyan.dev/ffmpeg/builds/
                                (the "essentials" build is enough)

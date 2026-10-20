# CMake generated Testfile for 
# Source directory: /Users/mohamedhassan/CLionProjects/Shader_Simulation
# Build directory: /Users/mohamedhassan/CLionProjects/Shader_Simulation/build-release
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("export_plan" "/Users/mohamedhassan/CLionProjects/Shader_Simulation/build-release/export_plan_tests")
set_tests_properties("export_plan" PROPERTIES  _BACKTRACE_TRIPLES "/Users/mohamedhassan/CLionProjects/Shader_Simulation/CMakeLists.txt;195;add_test;/Users/mohamedhassan/CLionProjects/Shader_Simulation/CMakeLists.txt;0;")
subdirs("_deps/glfw-build")
subdirs("_deps/nfd-build")
subdirs("_deps/json-build")
subdirs("_deps/glslang-build")
subdirs("_deps/spirvcross-build")

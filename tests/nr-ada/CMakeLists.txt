cmake_minimum_required(VERSION 3.21)
project(TRPNeuralKernelTests LANGUAGES CXX)
enable_testing()
option(TRP_ENABLE_OPTIONAL_FEATURES "Build the Universal RTX 20/30/40 kernel installer" ON)
set(ada_sources ../../src/FrameGen/NeuralRuntimeKernelsNative.cpp)
if(TRP_ENABLE_OPTIONAL_FEATURES)
    set(ada_sources ../../src/FrameGen/NeuralRuntimeKernels.cpp ../../src/FrameGen/NeuralRuntimeKernelPlan.cpp)
endif()
add_executable(TRPNeuralKernelTests AdaTests.cpp ${ada_sources})
target_compile_features(TRPNeuralKernelTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralKernelTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    $<$<BOOL:${TRP_ENABLE_OPTIONAL_FEATURES}>:TRP_ADA_UNIVERSAL>)
target_include_directories(TRPNeuralKernelTests PRIVATE ../../src)
target_link_libraries(TRPNeuralKernelTests PRIVATE bcrypt)
add_test(NAME NeuralRuntimeKernels COMMAND TRPNeuralKernelTests)
# Manual only: supplying a DLL and pack exercises actual Windows image mapping,
# pinning and publication. It never initializes NGX or launches the game.

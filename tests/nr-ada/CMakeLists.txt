cmake_minimum_required(VERSION 3.21)
project(TRPNeuralAdaTests LANGUAGES CXX)
enable_testing()
option(TRP_ENABLE_OPTIONAL_FEATURES "Build the Universal Ada kernel installer" ON)
set(ada_sources ../../src/FrameGen/NeuralRuntimeAdaKernelsNative.cpp)
if(TRP_ENABLE_OPTIONAL_FEATURES)
    set(ada_sources ../../src/FrameGen/NeuralRuntimeAdaKernels.cpp ../../src/FrameGen/NeuralRuntimeAdaKernelPlan.cpp)
endif()
add_executable(TRPNeuralAdaTests AdaTests.cpp ${ada_sources})
target_compile_features(TRPNeuralAdaTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralAdaTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    $<$<BOOL:${TRP_ENABLE_OPTIONAL_FEATURES}>:TRP_ADA_UNIVERSAL>)
target_include_directories(TRPNeuralAdaTests PRIVATE ../../src)
target_link_libraries(TRPNeuralAdaTests PRIVATE bcrypt)
add_test(NAME NeuralAdaKernels COMMAND TRPNeuralAdaTests)
# Manual only: supplying a DLL and pack exercises actual Windows image mapping,
# pinning and publication. It never initializes NGX or launches the game.

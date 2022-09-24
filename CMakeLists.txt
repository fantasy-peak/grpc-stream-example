cmake_minimum_required(VERSION 3.13.0)

project(example)

set(CMAKE_CXX_STANDARD 20)

set(OPTFLAGS -gdwarf-2)
set(WARNINGS -Wall -Wno-missing-field-initializers -Wno-ignored-qualifiers -g)
add_compile_options(${OPTFLAGS} ${WARNINGS})

if(NOT EXISTS "${CMAKE_BINARY_DIR}/xrepo.cmake")
    message(STATUS "Downloading xrepo.cmake from https://github.com/xmake-io/xrepo-cmake/")
    # mirror https://cdn.jsdelivr.net/gh/xmake-io/xrepo-cmake@main/xrepo.cmake
    file(DOWNLOAD "https://raw.githubusercontent.com/xmake-io/xrepo-cmake/main/xrepo.cmake"
                  "${CMAKE_BINARY_DIR}/xrepo.cmake"
                  TLS_VERIFY ON)
endif()

set(XREPO_PACKAGE_VERBOSE ON)
set(XREPO_BOOTSTRAP_XMAKE ON)
set(XMAKE_RELEASE_LATEST v2.7.1)
set(XREPO_BUILD_PARALLEL_JOBS 12)

include(${CMAKE_BINARY_DIR}/xrepo.cmake)
if(NOT EXISTS "$ENV{HOME}/.xmake/repositories/my_private_repo/xmake.lua")
    message(STATUS "add my_private_repo")
    execute_process(COMMAND xmake repo -a -g my_private_repo https://github.com/fantasy-peak/xmake-repo.git)
endif()

execute_process(COMMAND ${CMAKE_SOURCE_DIR}/bin/protoc 
    -I ${CMAKE_SOURCE_DIR}/bin 
    --cpp_out=${CMAKE_SOURCE_DIR}/grpc
    --grpc_out=${CMAKE_SOURCE_DIR}/grpc
    --plugin=protoc-gen-grpc=${CMAKE_SOURCE_DIR}/bin/grpc_cpp_plugin ${CMAKE_SOURCE_DIR}/bin/hellostreamingworld.proto)

#----------------------------------------------------------------------------------
set(XREPO_XMAKEFILE ${CMAKE_CURRENT_SOURCE_DIR}/repo/packages/g/grpc/xmake.lua)
xrepo_package("grpc v1.49.1" DEPS "abseil" "c-ares" "protobuf-cpp" "re2" "openssl" "zlib" )
find_package(gRPC CONFIG REQUIRED)
#-----------------------------------------------------------------------------------

include_directories(grpc)

add_executable(test_grpc_stream
    src/test_grpc_stream.cpp
    grpc/hellostreamingworld.grpc.pb.cc
    grpc/hellostreamingworld.pb.cc
)
xrepo_target_packages(test_grpc_stream PUBLIC grpc NO_LINK_LIBRARIES)
target_link_libraries(test_grpc_stream PUBLIC gRPC::gpr gRPC::upb gRPC::grpc gRPC::grpc++ ssl crypto pthread dl
)


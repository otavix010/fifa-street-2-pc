# FIFA Street 2 (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(fifast2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(FIFAStreet2 default.xbe game.toml ${GAME_PARTITIONS} game)

# map of the code DLL: the crash log gives the module offset of a fault, the map names the guest function
target_link_options(FIFAStreet2_code PRIVATE -Wl,/map:${CMAKE_CURRENT_BINARY_DIR}/FIFAStreet2.map)

# temporal anti-aliasing: NVIDIA DLSS (NGX, linked), AMD FSR and Intel XeSS (loaded at run time); their DLLs beside the exe
target_include_directories(FIFAStreet2 PRIVATE third_party/dlss/include third_party/ffx/api/include third_party/ffx/upscalers/include third_party/xess/include)
target_link_libraries(FIFAStreet2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dlss/lib/nvsdk_ngx_d.lib)
add_custom_command(TARGET FIFAStreet2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dlss/bin/nvngx_dlss.dll
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx/bin/amd_fidelityfx_loader_dx12.dll
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx/bin/amd_fidelityfx_upscaler_dx12.dll
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xess/bin/libxess.dll
        $<TARGET_FILE_DIR:FIFAStreet2>)

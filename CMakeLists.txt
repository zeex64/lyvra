cmake_minimum_required(VERSION 3.20)
project(lyvra VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(VULKAN REQUIRED vulkan)
# Headers are vendored (third_party/vulkan-headers) so only the loader is needed on the system.
add_library(vulkan_iface INTERFACE)
target_include_directories(vulkan_iface INTERFACE ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include)
target_link_libraries(vulkan_iface INTERFACE ${VULKAN_LINK_LIBRARIES})
find_package(Threads REQUIRED)
include(GNUInstallDirs)
find_program(GLSLC glslc REQUIRED)

# ---- Dear ImGui -------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC PkgConfig::SDL3 vulkan_iface)
include(cmake/fidelityfx.cmake)

# ---- Shaders -> embedded SPIR-V ---------------------------------------------
set(FSR1_DIR ${CMAKE_SOURCE_DIR}/third_party/fsr1/ffx-fsr)
set(SHADER_SRC yuv.comp easu.comp rcas.comp blit.vert blit.frag motion.comp jitter.comp jitter16.comp fgmotion.comp fglock.comp fgcheck.comp)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})
foreach(s ${SHADER_SRC})
  set(out ${SHADER_OUT_DIR}/${s}.inc)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${GLSLC} -O --target-env=vulkan1.3 -I${FSR1_DIR} -MD -MF ${out}.d -mfmt=c
            -o ${out} ${CMAKE_SOURCE_DIR}/shaders/${s}
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${s}
    DEPFILE ${out}.d
    COMMENT "glslc ${s}")
  list(APPEND SHADER_OUTS ${out})
endforeach()
add_custom_target(lyvra_shaders DEPENDS ${SHADER_OUTS})

# ---- Fonts -> embedded byte arrays ------------------------------------------
set(FONT_NAMES Inter-latin-400-normal Inter-latin-600-normal Inter-latin-700-normal
               JetBrainsMono-latin-400-normal JetBrainsMono-latin-600-normal)
set(FONT_FILES)
foreach(f ${FONT_NAMES})
  list(APPEND FONT_FILES ${CMAKE_SOURCE_DIR}/assets/fonts/${f}.ttf)
endforeach()
set(FONTS_CPP ${CMAKE_BINARY_DIR}/embedded_fonts.cpp)
string(REPLACE ";" "," FONT_NAMES_ARG "${FONT_NAMES}")
add_custom_command(
  OUTPUT ${FONTS_CPP}
  COMMAND ${CMAKE_COMMAND} -DOUT=${FONTS_CPP} -DDIR=${CMAKE_SOURCE_DIR}/assets/fonts
          "-DNAMES=${FONT_NAMES_ARG}" -P ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.cmake
  DEPENDS ${FONT_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.cmake
  COMMENT "Embedding fonts")

# ---- App icon -> embedded PNG (window icon) ----------------------------------
set(ICON_CPP ${CMAKE_BINARY_DIR}/embedded_icon.cpp)
add_custom_command(
  OUTPUT ${ICON_CPP}
  COMMAND ${CMAKE_COMMAND} -DOUT=${ICON_CPP} -DFILE=${CMAKE_SOURCE_DIR}/data/icons/lyvra-128.png -DNAME=app_icon_png
          -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/data/icons/lyvra-128.png ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  COMMENT "Embedding app icon")

# ---- App --------------------------------------------------------------------
add_executable(lyvra
  src/main.cpp src/capture.cpp src/renderer.cpp src/ui.cpp src/audio.cpp
  src/settings.cpp src/fsr1_constants.cpp src/temporal.cpp src/framegen.cpp ${FONTS_CPP} ${ICON_CPP})
add_dependencies(lyvra lyvra_shaders)
target_include_directories(lyvra PRIVATE src ${SHADER_OUT_DIR} ${FSR1_DIR}
  ${CMAKE_SOURCE_DIR}/third_party/ffx-api/api/include ${CMAKE_SOURCE_DIR}/third_party/ffx-api/upscalers/include)
target_compile_options(lyvra PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(lyvra PRIVATE ffx_sdk imgui PkgConfig::SDL3 vulkan_iface Threads::Threads ${CMAKE_DL_LIBS})

# FSR 4: fsr4-vk (GPL-3.0, third_party/fsr4-vk) builds amd_fidelityfx_upscaler_vk.so, which Lyvra loads
# at runtime through the FFX API. Point this at a built copy to place it next to the binary.
set(LYVRA_FSR4_VK_LIB "" CACHE FILEPATH "Path to fsr4-vk's amd_fidelityfx_upscaler_vk.so (optional)")
if(LYVRA_FSR4_VK_LIB)
  add_custom_command(TARGET lyvra POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LYVRA_FSR4_VK_LIB} $<TARGET_FILE_DIR:lyvra>/amd_fidelityfx_upscaler_vk.so
    COMMENT "Copying fsr4-vk library")
  install(FILES ${LYVRA_FSR4_VK_LIB} DESTINATION ${CMAKE_INSTALL_LIBDIR}/lyvra RENAME amd_fidelityfx_upscaler_vk.so)
endif()

include(GNUInstallDirs)
install(TARGETS lyvra RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/lyvra.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/icons/lyvra.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
foreach(sz 16 32 48 64 128 256 512)
  install(FILES data/icons/lyvra-${sz}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${sz}x${sz}/apps RENAME lyvra.png)
endforeach()

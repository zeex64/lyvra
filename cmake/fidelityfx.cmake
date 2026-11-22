# ---- AMD FidelityFX SDK 1.1.4 (Linux / Vulkan) --------------------------------
# Builds a static `ffx_sdk` library with only FSR 3.1 upscaling, optical flow and
# frame interpolation on the Vulkan backend, plus:
#   ffx_sc      host tool: Linux/GLSL-only port of FidelityFX_SC (tools/ffx_sc)
#   ffx_sdk_shaders  custom target generating *_permutations.h (SPIR-V + reflection)
#   ffx_smoke   headless smoke test (tools/ffx_smoke), not installed
# Nothing under third_party/ is modified. Linux fixes live here instead:
#   cmake/ffx_linux_compat.h        forced include (MSVC CRT *_s, _countof, ...)
#   cmake/ffx_include/...           shadowed ffx_types.h (context size) + ffx_util.h (<bit>)
#   cmake/ffx_patched/ffx_message.cpp  forwards FFX messages to the callback on Linux
#   cmake/ffx_vk_swapchain_stub.cpp FrameInterpolationSwapchain excluded (one stub)
#   ${build}/ffx_patched/ffx_vk.cpp configure-time patched copy (alignment fix)
# Consumers must enable VK_KHR_get_memory_requirements2 on the device (see below).

set(FFX_SDK_DIR    ${CMAKE_SOURCE_DIR}/third_party/fidelityfx/sdk)
set(FFX_GPU_PATH   ${FFX_SDK_DIR}/include/FidelityFX/gpu)
set(FFX_VK_DIR     ${FFX_SDK_DIR}/src/backends/vk)
set(FFX_SC_LIB_DIR ${FFX_SDK_DIR}/tools/ffx_shader_compiler/libs)

find_program(FFX_GLSLANG_VALIDATOR glslangValidator REQUIRED)

# ---- ffx_sc: shader permutation compiler (host tool) -------------------------
add_executable(ffx_sc
  ${CMAKE_SOURCE_DIR}/tools/ffx_sc/ffx_sc.cpp
  ${CMAKE_SOURCE_DIR}/tools/ffx_sc/glsl_compiler.cpp
  ${CMAKE_SOURCE_DIR}/tools/ffx_sc/utils.cpp
  ${FFX_SC_LIB_DIR}/MD5/md5.cpp
  ${FFX_SC_LIB_DIR}/SPIRV-Reflect/spirv_reflect.c
  ${FFX_SC_LIB_DIR}/tiny-process-library/process.cpp
  ${FFX_SC_LIB_DIR}/tiny-process-library/process_unix.cpp)
target_include_directories(ffx_sc PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/ffx_sc
  ${FFX_SC_LIB_DIR}/MD5
  ${FFX_SC_LIB_DIR}/SPIRV-Reflect
  ${FFX_SC_LIB_DIR}/tiny-process-library)
target_link_libraries(ffx_sc PRIVATE Threads::Threads)
target_compile_options(ffx_sc PRIVATE -w)

# ---- Shader permutation headers ---------------------------------------------
# Replicates sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt for GLSL: four
# headers per pass. For GLSL the SDK adds no wave-size or 16-bit compiler flags;
# the _wave64 variants differ only by -name and the _16bit ones by -DFFX_HALF=1.
set(FFX_SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/ffx_shaders)
file(MAKE_DIRECTORY ${FFX_SHADER_OUT_DIR})

cmake_host_system_information(RESULT _ffx_ncpu QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR _ffx_threads "${_ffx_ncpu} / 4")
if(_ffx_threads LESS 2)
  set(_ffx_threads 2)
endif()
set(FFX_SC_THREADS ${_ffx_threads} CACHE STRING "glslangValidator processes per ffx_sc invocation")
set(FFX_SC_JOBS 8 CACHE STRING "Max concurrent ffx_sc invocations (Ninja job pool)")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS ffx_sc_pool=${FFX_SC_JOBS})

set(FFX_SC_API_ARGS -compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1)

# ffx_sc_compile(<out-list-var> BASE_ARGS ... PERMUTATION_ARGS ... INCLUDES ... SHADERS ...)
function(ffx_sc_compile OUT_VAR)
  cmake_parse_arguments(A "" "" "BASE_ARGS;PERMUTATION_ARGS;INCLUDES;SHADERS" ${ARGN})
  set(inc_args)
  foreach(inc ${A_INCLUDES})
    list(APPEND inc_args "-I${inc}")
  endforeach()
  # Ninja runs commands through /bin/sh (bash on Arch), which brace-expands an
  # unquoted -DOPT={0,1} into -DOPT=0 -DOPT=1, and CMake does not quote braces.
  # ffx_sc re-joins argv words until it sees '}', so pass "-DOPT={0," "1}" instead:
  # neither word is a valid brace expression and the tool sees the same option.
  set(perm_args)
  foreach(p ${A_PERMUTATION_ARGS})
    if(p MATCHES "^([^{]*{[^,]*,)(.+)$")
      list(APPEND perm_args "${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    else()
      list(APPEND perm_args "${p}")
    endif()
  endforeach()
  set(sc_args ${A_BASE_ARGS} ${FFX_SC_API_ARGS} ${perm_args})
  set(outs)
  foreach(shader ${A_SHADERS})
    get_filename_component(name ${shader} NAME_WE)
    #          suffix          FFX_HALF
    foreach(variant "|0" "_wave64|0" "_16bit|1" "_wave64_16bit|1")
      string(REPLACE "|" ";" variant "${variant}")
      list(GET variant 0 suffix)
      list(GET variant 1 half)
      set(hdr ${FFX_SHADER_OUT_DIR}/${name}${suffix}_permutations.h)
      add_custom_command(
        OUTPUT ${hdr}
        COMMAND ffx_sc ${sc_args} -name=${name}${suffix} -DFFX_HALF=${half} ${inc_args}
                -output=${FFX_SHADER_OUT_DIR} -glslangexe=${FFX_GLSLANG_VALIDATOR}
                -num-threads=${FFX_SC_THREADS} ${shader}
        DEPENDS ${shader} ffx_sc
        DEPFILE ${hdr}.d
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        JOB_POOL ffx_sc_pool
        COMMENT "FidelityFX-SC ${name}${suffix}"
        VERBATIM)
      list(APPEND outs ${hdr})
    endforeach()
  endforeach()
  set(${OUT_VAR} ${outs} PARENT_SCOPE)
endfunction()

# FSR3 upscaler (sdk/include/FidelityFX/gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt)
file(GLOB FFX_FSR3UPSCALER_SHADERS ${FFX_VK_DIR}/shaders/fsr3upscaler/*.glsl)
ffx_sc_compile(FFX_FSR3UPSCALER_HEADERS
  BASE_ARGS -reflection -deps=gcc -DFFX_GPU=1
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
    -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
  PERMUTATION_ARGS
    -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}
    -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}
    -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}
    -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}
    -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}
    -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}
  INCLUDES ${FFX_GPU_PATH} ${FFX_GPU_PATH}/fsr3upscaler
  SHADERS ${FFX_FSR3UPSCALER_SHADERS})

# Optical flow (sdk/include/FidelityFX/gpu/opticalflow/CMakeCompileOpticalflowShaders.txt)
set(FFX_OPTICALFLOW_SHADERS)
foreach(s prepare_luma_pass compute_luminance_pyramid_pass generate_scd_histogram_pass
          compute_scd_divergence_pass compute_optical_flow_advanced_pass_v5
          filter_optical_flow_pass_v5 scale_optical_flow_advanced_pass_v5)
  list(APPEND FFX_OPTICALFLOW_SHADERS ${FFX_VK_DIR}/shaders/opticalflow/ffx_opticalflow_${s}.glsl)
endforeach()
ffx_sc_compile(FFX_OPTICALFLOW_HEADERS
  BASE_ARGS -reflection -deps=gcc -DFFX_GPU=1
  PERMUTATION_ARGS -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}
  INCLUDES ${FFX_GPU_PATH} ${FFX_GPU_PATH}/opticalflow
  SHADERS ${FFX_OPTICALFLOW_SHADERS})

# Frame interpolation (sdk/include/FidelityFX/gpu/frameinterpolation/CMakeCompileFrameinterpolationShaders.txt)
file(GLOB FFX_FRAMEINTERPOLATION_SHADERS ${FFX_VK_DIR}/shaders/frameinterpolation/*.glsl)
ffx_sc_compile(FFX_FRAMEINTERPOLATION_HEADERS
  BASE_ARGS
    -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
    -DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
    -reflection -deps=gcc -DFFX_GPU=1
  PERMUTATION_ARGS
    -DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={0,1}
    -DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS={0,1}
    -DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={0,1}
  INCLUDES ${FFX_GPU_PATH} ${FFX_GPU_PATH}/frameinterpolation
  SHADERS ${FFX_FRAMEINTERPOLATION_SHADERS})

add_custom_target(ffx_sdk_shaders DEPENDS
  ${FFX_FSR3UPSCALER_HEADERS} ${FFX_OPTICALFLOW_HEADERS} ${FFX_FRAMEINTERPOLATION_HEADERS})

# ---- ffx_vk.cpp: configure-time patch -------------------------------------------
# BackendContext_VK::EffectContext is alignas(32), but ffx_vk.cpp carves it out of
# the scratch buffer at an offset that is only 4-byte aligned. With MSVC's (2-byte
# wchar_t) struct sizes the offset happens to be aligned; with GCC it is not, and the
# vectorised stores into it (movaps) segfault on first context creation. Patch a copy
# in the build tree: align that pointer and add 32 bytes of slack to the scratch size.
set(FFX_VK_PATCHED ${CMAKE_BINARY_DIR}/ffx_patched/ffx_vk.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FFX_VK_DIR}/ffx_vk.cpp)
file(READ ${FFX_VK_DIR}/ffx_vk.cpp _ffx_vk_src)
macro(_ffx_vk_patch FROM TO)
  string(FIND "${_ffx_vk_src}" "${FROM}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "fidelityfx.cmake: ffx_vk.cpp patch anchor not found: ${FROM}")
  endif()
  string(REPLACE "${FROM}" "${TO}" _ffx_vk_src "${_ffx_vk_src}")
endmacro()
_ffx_vk_patch(
  "return FFX_ALIGN_UP(sizeof(BackendContext_VK) + extensionPropArraySize"
  "return FFX_ALIGN_UP(32 /* lyvra: EffectContext alignment slack */ + sizeof(BackendContext_VK) + extensionPropArraySize")
_ffx_vk_patch(
  "backendContext->pEffectContexts = (BackendContext_VK::EffectContext*)pMem"
  "backendContext->pEffectContexts = (BackendContext_VK::EffectContext*)(pMem = (uint8_t*)FFX_ALIGN_UP((uintptr_t)pMem, (uintptr_t)alignof(BackendContext_VK::EffectContext))) /* lyvra: align */")
file(WRITE ${FFX_VK_PATCHED}.tmp "// GENERATED by cmake/fidelityfx.cmake from third_party/fidelityfx/sdk/src/backends/vk/ffx_vk.cpp -- do not edit.\n${_ffx_vk_src}")
configure_file(${FFX_VK_PATCHED}.tmp ${FFX_VK_PATCHED} COPYONLY)

# ---- ffx_sdk static library -------------------------------------------------
set(FFX_SRC ${FFX_SDK_DIR}/src)
add_library(ffx_sdk STATIC
  ${FFX_SRC}/components/fsr3upscaler/ffx_fsr3upscaler.cpp
  ${FFX_SRC}/components/opticalflow/ffx_opticalflow.cpp
  ${FFX_SRC}/components/frameinterpolation/ffx_frameinterpolation.cpp
  ${FFX_SRC}/shared/ffx_assert.cpp
  ${CMAKE_SOURCE_DIR}/cmake/ffx_patched/ffx_message.cpp  # Linux: forward messages to callback
  ${FFX_SRC}/shared/ffx_object_management.cpp
  ${FFX_SRC}/shared/ffx_breadcrumbs_list.cpp
  ${FFX_SRC}/backends/shared/ffx_shader_blobs.cpp
  ${FFX_SRC}/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
  ${FFX_SRC}/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp
  ${FFX_SRC}/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp
  ${FFX_VK_PATCHED}                                      # sdk/src/backends/vk/ffx_vk.cpp + alignment fix
  ${CMAKE_SOURCE_DIR}/cmake/ffx_vk_swapchain_stub.cpp)
add_dependencies(ffx_sdk ffx_sdk_shaders)
target_include_directories(ffx_sdk
  # cmake/ffx_include shadows FidelityFX/host/ffx_types.h (larger opaque context
  # size for 4-byte wchar_t); it must stay ahead of sdk/include for consumers too.
  PUBLIC  ${CMAKE_SOURCE_DIR}/cmake/ffx_include ${FFX_SDK_DIR}/include ${FFX_SHADER_OUT_DIR}
  PRIVATE ${FFX_SRC}/components ${FFX_SRC}/shared ${FFX_SRC}/backends/shared)
# ffx_shader_blobs.cpp only compiles in the accessors whose FFX_<EFFECT> macro is set.
target_compile_definitions(ffx_sdk
  PUBLIC  FFX_GCC=1
  PRIVATE FFX_FSR3UPSCALER FFX_FI FFX_OF _UNICODE UNICODE)
target_compile_options(ffx_sdk PRIVATE
  "SHELL:-include ${CMAKE_SOURCE_DIR}/cmake/ffx_linux_compat.h"
  -fno-strict-aliasing
  -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers
  -Wno-switch -Wno-reorder -Wno-unknown-pragmas -Wno-narrowing -Wno-format
  -Wno-class-memaccess -Wno-deprecated-declarations -Wno-unused-value
  -Wno-int-to-pointer-cast -Wno-pointer-arith -Wno-parentheses
  -Wno-stringop-truncation -Wno-stringop-overflow -Wno-array-bounds -Wno-overflow)
set_target_properties(ffx_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ffx_sdk PUBLIC vulkan_iface Threads::Threads)
# Device requirements for apps using ffx_sdk (tools/ffx_smoke/main.cpp shows them):
#  - device extension VK_KHR_get_memory_requirements2 (+ VK_KHR_dedicated_allocation):
#    ffx_vk.cpp calls vkGetBufferMemoryRequirements2KHR via vkGetDeviceProcAddr on the
#    first context creation; without the extension enabled that pointer is NULL.
#  - features: shaderFloat16 + shaderInt16 (the backend picks the 16-bit permutations
#    whenever the device *supports* fp16), shaderStorageImageReadWithoutFormat,
#    shaderStorageImageWriteWithoutFormat; subgroup basic/quad/arithmetic in compute.

# ---- ffx_smoke: headless context create/destroy test (not installed) --------
add_executable(ffx_smoke ${CMAKE_SOURCE_DIR}/tools/ffx_smoke/main.cpp)
target_link_libraries(ffx_smoke PRIVATE ffx_sdk)

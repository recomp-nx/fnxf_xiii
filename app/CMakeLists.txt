# fnxf_xiii: an Xbox 360 game recompiled with ReXGlue, for the Nintendo Switch (only: no PC build since 2026-10-29).
#
# The renderer: reNut's Direct3D-level renderer through plume's deko3d back end.

cmake_minimum_required(VERSION 3.25)
project(fnxf_xiii LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT REXGLUE_PLATFORM_SWITCH)
    message(FATAL_ERROR "fnxf_xiii builds for the Nintendo Switch only: configure with "
                        "tools/switch/cmake/switch-devkitA64.cmake (build_switch.py does it)")
endif()

# Switch toolchain flags (see the measurements behind them in the project's history).
add_compile_options(-mearly-ra=none -mno-outline-atomics)
add_link_options(-mearly-ra=none)

# fnxf_xiii (2026-10-30): the optimisation level and link-time optimisation (build_switch.py --opt O2|O3 --lto, the
# build_switch_o2_lto.bat / build_switch_o3_lto.bat builds, each in a folder of its own). Set before the SDK is added,
# so its targets get them too. FNXF_XIII_OPT empty: as before (-O3: CMake's Release flags and the SDK's own).
# uam keeps its own -O2 (its CMakeLists), the function registration its -O0 (below). With FNXF_XIII_LTO, every C and C++
# object but uam's (thirdparty/plume/CMakeLists.txt: Mesa's front end came out wrong with it) holds GCC's intermediate
# code (no machine code: -flto's default, no fat objects) and the NRO's link compiles
# the whole program at once, with the same level (-flto=auto: the link uses as many jobs as there are cores), and
# -fipa-pta. With PGO: the counters are placed when each file is compiled, so a profile made at the same level without
# LTO fits (build_switch.py: pgo/ for -O3, pgo-o2/ for -O2).
set(FNXF_XIII_OPT "" CACHE STRING "Optimisation level of every target: O2, O3, or empty (O3, as before)")
option(FNXF_XIII_LTO "Link-time optimisation of the whole NRO" OFF)
if(FNXF_XIII_OPT)
    if(NOT FNXF_XIII_OPT MATCHES "^O[23]$")
        message(FATAL_ERROR "FNXF_XIII_OPT is O2, O3 or empty, not '${FNXF_XIII_OPT}'")
    endif()
    set(CMAKE_C_FLAGS_RELEASE "-${FNXF_XIII_OPT} -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-${FNXF_XIII_OPT} -DNDEBUG")
    set(REXGLUE_RELEASE_OPT "-${FNXF_XIII_OPT}")
endif()
set(FNXF_XIII_OPT_FLAG "-O3")
if(FNXF_XIII_OPT)
    set(FNXF_XIII_OPT_FLAG "-${FNXF_XIII_OPT}")
endif()
if(FNXF_XIII_LTO)
    # -fipa-pta: points-to analysis across functions (and, at the LTO link, across the whole program): which memory a
    # pointer can reach, so loads and stores through others may move past it. Its cost grows with the program: the
    # link's time and memory above all.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-flto=auto> $<$<COMPILE_LANGUAGE:C,CXX>:-fipa-pta>)
    # The link compiles the code: the same level, and the options the results depend on given again (GCC keeps them
    # with each function too): no fused multiply-add (the guest's float results), no outline atomics.
    add_link_options(-flto=auto ${FNXF_XIII_OPT_FLAG} -fipa-pta -ffp-contract=off -fno-strict-aliasing
                     -mno-outline-atomics)
endif()
message(STATUS "fnxf_xiii: optimisation ${FNXF_XIII_OPT_FLAG}, link-time optimisation ${FNXF_XIII_LTO}")

# fnxf_xiii (2026-10-21): profile-guided optimisation (src/pgo_switch.h). FNXF_XIII_PGO=generate: every target (set before the SDK is added, so its targets too) compiled
# and linked with -fprofile-generate (the counts go to /switch/fnxf_xiii/pgo on the SD card); FNXF_XIII_PGO=use: with
# -fprofile-use from FNXF_XIII_PGO_DIR (the folder copied back from the console), in the same build folder (GCC names a
# profile after the object's path).
set(FNXF_XIII_PGO "" CACHE STRING "Profile-guided optimisation: generate, use or empty")
set(FNXF_XIII_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "The profile (FNXF_XIII_PGO=use)")
# fnxf_xiii (2026-10-25): the prefix as GCC sees the object's path. GCC compares it byte for byte with its working
# folder (getpwd: backslashes on Windows) joined to the object's name, so a prefix with forward slashes never matched
# there ("does not start with profile prefix"): every profile kept the whole path, out#switch-pgo#... in the profiling
# build, which the release in out/switch then looked for as out#switch#... and never found.
if(CMAKE_HOST_WIN32)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" FNXF_XIII_PGO_PREFIX)
else()
    set(FNXF_XIII_PGO_PREFIX "${CMAKE_BINARY_DIR}")
endif()
# fnxf_xiii (2026-10-26): the profiling build numbers its functions inside their file (the same wherever the project
# is) and writes each object's function list (-ftest-coverage: a .gcno beside it, the code unchanged); the release
# keeps GCC's own ids (a hash of the name, and for a function not visible outside its file also of its source's path),
# and tools/pgo_unpack.py gives every function of the profile the id the release looks for (tools/pgo_functions.py).
# The release cannot number them too: GCC crashes at -fprofile-use on a file with a C++ thunk when it does
# (read_thunk_profile; threading_posix.cpp, "internal compiler error: Segmentation fault").
set(FNXF_XIII_PGO_ID --param=profile-func-internal-id=1 -ftest-coverage)
if(FNXF_XIII_PGO STREQUAL "generate")
    # 2026-10-22: no value profiling (-fno-profile-values): its indirect-call profiler in devkitA64's prebuilt libgcov
    # reads its thread-local through TPIDR_EL0, which Horizon does not set for homebrew (libnx's TLS is behind
    # TPIDRRO_EL0, -mtp=soft): the first indirect call faulted and the instrumented build hung before its log
    # fnxf_xiii (2026-10-24): -fprofile-prefix-path: each profile named after its object's path inside the build folder
    # (a#b#c.gcda), not the whole path of this PC: the profiling build has a folder of its own (build_switch.py --pgo
    # generate: out/switch-pgo) and the release finds the counts by the same names in out/switch.
    add_compile_options(-fprofile-generate=/switch/fnxf_xiii/pgo -fprofile-update=prefer-atomic -fno-profile-values
                        -fprofile-prefix-path=${FNXF_XIII_PGO_PREFIX} ${FNXF_XIII_PGO_ID})
    # fnxf_xiii (2026-10-25): every object's profile registered with src/pgo_switch.cpp instead of libgcov (one file per
    # run written there; libgcov's one file per object never finished on the SD card).
    add_link_options(-fprofile-generate=/switch/fnxf_xiii/pgo -fno-profile-values -Wl,--wrap=__gcov_init)
    add_compile_definitions(FNXF_XIII_PGO_GENERATE=1)
    # fnxf_xiii (2026-10-30): where src/pgo_switch.cpp writes its runs (the -O2 profiling build: /switch/fnxf_xiii/pgo-o2,
    # build_switch.py). The counters' own names keep /switch/fnxf_xiii/pgo (tools/pgo_unpack.py reads them so).
    set(FNXF_XIII_PGO_RUN_DIR "/switch/fnxf_xiii/pgo" CACHE STRING "The SD card's folder for the profiling build's runs")
    add_compile_definitions(FNXF_XIII_PGO_RUN_DIR="${FNXF_XIII_PGO_RUN_DIR}")
elseif(FNXF_XIII_PGO STREQUAL "use")
    # 2026-10-25: -Wno-missing-profile dropped: an object without its counts is now reported (a path mismatch showed
    # nothing before).
    # fnxf_xiii (2026-10-26): -fprofile-correction: counts that do not add up (a branch's edges against its block's
    # count) are smoothed instead of stopping the build ("corrupted profile info: profile data is not flow-consistent",
    # glcpp-parse.y): the pre-pass part scaled down by tools/pgo_unpack.py rounds each count on its own, its snapshot
    # was taken while threads ran, and the counters of code several threads run are not updated as one.
    add_compile_options(-fprofile-use=${FNXF_XIII_PGO_DIR} -fprofile-partial-training -fprofile-correction
                        -fno-profile-values -Wno-coverage-mismatch -fprofile-prefix-path=${FNXF_XIII_PGO_PREFIX})
    add_link_options(-fprofile-use=${FNXF_XIII_PGO_DIR} -fprofile-partial-training -fprofile-correction
                     -fno-profile-values)
    # fnxf_xiii (2026-10-26): the profile's id (build_switch.py: from pgo/'s runs) in every compile line: a new
    # profile compiles everything again with it, the same one nothing (the .gcda files are not in ninja's dependencies).
    if(FNXF_XIII_PGO_PROFILE_ID)
        add_compile_definitions(FNXF_XIII_PGO_PROFILE_ID=${FNXF_XIII_PGO_PROFILE_ID})
    endif()
endif()

include(generated/rexglue.cmake)
# fnxf_xiii (2026-10-26): the function registration (one function of about 44 000 calls, run once at start) without
# optimisation: GCC's -O2 on it took 81 minutes on one core, the build's longest step by far. Appended to the generated
# sources' options, so it comes after -O2 and wins. Without the precompiled header (made with -O2, GCC refuses it for
# -O0: "not used because '__OPTIMIZE__' not defined"); the file includes the same headers itself (fnxf_xiii_init.h).
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fnxf_xiii_register.cpp" APPEND PROPERTY COMPILE_OPTIONS -O0)
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fnxf_xiii_register.cpp" PROPERTY SKIP_PRECOMPILE_HEADERS ON)

set(FNXF_XIII_SOURCES
    src/main.cpp
    src/cpu/fnxf_xiii_enhancements.cpp
    src/fnxf_xiii_renderer_shared.cpp
    src/fnxf_xiii_stall_dump.cpp
    src/fnxf_xiii_spin_barrier.cpp
    src/fnxf_xiii_slot_wait_diag.cpp
    src/cpu/fnxf_xiii_native_zlib.cpp
    src/cpu/fnxf_xiii_native_crt.cpp
    src/cpu/fnxf_xiii_native_vmath.cpp
    src/cpu/fnxf_xiii_native_audio.cpp
    src/cpu/fnxf_xiii_sync_diag.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_diag_sync_waits
    src/cpu/fnxf_xiii_job_diag.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_diag_jobs
    src/cpu/fnxf_xiii_native_locks.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_native_game_locks
    src/cpu/fnxf_xiii_native_vector.cpp  # fnxf_xiii (2026-11-05): fnxf_xiii_native_vector, the byte-vector fill-insert
    src/cpu/fnxf_xiii_mover_diag.cpp  # fnxf_xiii (2026-11-06): fnxf_xiii_diag_block_mover
    src/cpu/fnxf_xiii_parallel_memmove.cpp  # fnxf_xiii (2026-11-06): the game's large memmoves on 3 cores
    src/fnxf_xiii_romfs.cpp  # fnxf_xiii (2026-11-06): the port's own files in the NRO's romfs
    src/cpu/fnxf_xiii_render_resolution.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_render_720p
    src/cpu/fnxf_xiii_texture_sources.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_diag_texture_sources
    src/cpu/fnxf_xiii_skip_boot_prompts.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_skip_boot_prompts
    src/cpu/fnxf_xiii_disc_swap.cpp  # fnxf_xiii (2026-10-31): the disc swaps, every disc in the game folder
    src/cpu/fnxf_xiii_game_log.cpp  # fnxf_xiii (2026-11-01): fnxf_xiii_diag_game_log, the game's own debug messages
    src/cpu/fnxf_xiii_dflags_diag.cpp  # fnxf_xiii (2026-11-01): fnxf_xiii_diag_dflags, the game's debug switches
    src/cpu/fnxf_xiii_load_fade_fix.cpp  # fnxf_xiii (2026-11-09): fnxf_xiii_fix_load_fade, the unplaced camera after a load
    src/cpu/fnxf_xiii_touch_diag.cpp  # fnxf_xiii (2026-11-01): fnxf_xiii_diag_touch_off, field collision bodies kept off
    src/cpu/fnxf_xiii_ai_axis_diag.cpp  # fnxf_xiii (2026-11-01): fnxf_xiii_diag_ai_axis, the field AI's angle function
    src/cpu/fnxf_xiii_mesh_proxy_diag.cpp  # fnxf_xiii (2026-11-01): fnxf_xiii_diag_mesh_proxy, the physics reading model vertices
    src/cpu/fnxf_xiii_move_floor_diag.cpp  # fnxf_xiii (2026-11-01): fnxf_xiii_diag_move_floor, the boat's clips and bodies
    src/cpu/fnxf_xiii_switch_texts.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_switch_device_name
    src/cpu/fnxf_xiii_menu_trim.cpp  # fnxf_xiii (2026-11-11): the title menu without Extras
    src/cpu/fnxf_xiii_lang_pack.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_lang (lang/<name>.zip)
    src/cpu/fnxf_xiii_glyphs.cpp  # fnxf_xiii (2026-10-25): the language pack's characters in the game's fonts
    src/cpu/fnxf_xiii_texts.cpp  # fnxf_xiii (2026-10-25): the language pack's texts
    src/cpu/fnxf_xiii_images.cpp  # fnxf_xiii (2026-10-25): the language pack's pictures
    src/cpu/fnxf_xiii_layouts.cpp  # fnxf_xiii (2026-10-25): the language pack's UI layout changes
    src/cpu/fnxf_xiii_wpd_patch.cpp  # fnxf_xiii (2026-10-25): changes to the game's WPD files as they load
    src/cpu/fnxf_xiii_switch_buttons.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_switch_buttons
    src/cpu/fnxf_xiii_handheld_gpu.cpp  # fnxf_xiii (2026-10-25): fnxf_xiii_handheld_gpu_460
    # fnxf_xiii (2026-10-24): the CPU profiler (fnxf_xiii_cpu_profiler: the sampler, slow-frame reports, stall watchdog)
    src/profiler/profiler.cpp
    src/profiler/cpu_profiler_switch.cpp
    src/profiler/stall_watch_switch.cpp
)

if(FNXF_XIII_PGO STREQUAL "generate")
    list(APPEND FNXF_XIII_SOURCES src/pgo_switch.cpp)
endif()

add_executable(fnxf_xiii ${FNXF_XIII_SOURCES})

# fnxf_xiii (2026-11-06): the game's heap compaction walk sub_8279A0B8 with a byte budget (cpu/fnxf_xiii_mover_diag.cpp):
# made here from the generated function itself, so it always matches the code generation: the same code under another
# name, plus one test at the loop's continue point (loc_8279A640, after a block is done: stop when the budget is spent,
# leaving as at the list's end). Configuring fails if the function no longer has that shape.
# The generated code is one file a function, or (build_switch.py --batched) about 1 MB files of many: the file holding
# the function is looked for, and the function alone taken out of it (to its closing brace at the line start), with
# its include and the declarations of the functions it calls.
set(_fnxf_walk_src "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fnxf_xiii_recomp_8279A0B8.cpp")
if(NOT EXISTS "${_fnxf_walk_src}")
  set(_fnxf_walk_src "")
  file(GLOB _fnxf_walk_files "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fnxf_xiii_recomp*.cpp")
  foreach(_f IN LISTS _fnxf_walk_files)
    file(STRINGS "${_f}" _hit REGEX "^DEFINE_REX_FUNC\\(sub_8279A0B8\\) \\{" LIMIT_COUNT 1)
    if(_hit)
      set(_fnxf_walk_src "${_f}")
      break()
    endif()
  endforeach()
  if(NOT _fnxf_walk_src)
    message(FATAL_ERROR "fnxf_xiii_budgeted_walk: no generated file defines sub_8279A0B8 (run the code generation first)")
  endif()
endif()
set(_fnxf_walk_out "${CMAKE_BINARY_DIR}/fnxf_xiii_budgeted_walk.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fnxf_walk_src}")
file(READ "${_fnxf_walk_src}" _fnxf_file)
string(REPLACE "\r\n" "\n" _fnxf_file "${_fnxf_file}")  # line ends as written on Windows
string(FIND "${_fnxf_file}" "DEFINE_REX_FUNC(sub_8279A0B8) {" _at)
if(_at EQUAL -1)
  message(FATAL_ERROR "fnxf_xiii_budgeted_walk: 'DEFINE_REX_FUNC(sub_8279A0B8) {' not in ${_fnxf_walk_src} (the walk changed; see cpu/fnxf_xiii_mover_diag.cpp)")
endif()
string(SUBSTRING "${_fnxf_file}" ${_at} -1 _fnxf_walk)
string(FIND "${_fnxf_walk}" "\n}\n" _end)
if(_end EQUAL -1)
  message(FATAL_ERROR "fnxf_xiii_budgeted_walk: no end of sub_8279A0B8 in ${_fnxf_walk_src}")
endif()
math(EXPR _end "${_end} + 3")
string(SUBSTRING "${_fnxf_walk}" 0 ${_end} _fnxf_walk)
foreach(_needle "loc_8279A640:\n" "loc_8279A64C:")
  string(FIND "${_fnxf_walk}" "${_needle}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "fnxf_xiii_budgeted_walk: '${_needle}' not in sub_8279A0B8 (the walk changed; see cpu/fnxf_xiii_mover_diag.cpp)")
  endif()
endforeach()
string(REGEX MATCHALL "sub_[0-9A-F]+\\(ctx, base\\)" _calls "${_fnxf_walk}")
set(_decls "")
foreach(_c IN LISTS _calls)
  string(REGEX REPLACE "\\(ctx, base\\)" "" _c "${_c}")
  string(FIND "${_decls}" "(${_c})" _known)
  if(_known EQUAL -1)
    string(APPEND _decls "DECLARE_REX_FUNC(${_c});\n")
  endif()
endforeach()
set(_fnxf_walk "#include \"fnxf_xiii_pch.h\"\n\n${_decls}\n${_fnxf_walk}")
string(REPLACE "DEFINE_REX_FUNC(sub_8279A0B8) {"
       "bool FnxfXiiiMoverBudgetSpent();\nvoid FnxfXiiiBudgetedWalk([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base) {"
       _fnxf_walk "${_fnxf_walk}")
string(REPLACE "loc_8279A640:\n"
       "loc_8279A640:\n\tif (FnxfXiiiMoverBudgetSpent()) goto loc_8279A64C;  // fnxf_xiii: the budget\n"
       _fnxf_walk "${_fnxf_walk}")
get_filename_component(_fnxf_walk_name "${_fnxf_walk_src}" NAME)
set(_fnxf_walk "// Generated by app/CMakeLists.txt from sub_8279A0B8 in generated/default/${_fnxf_walk_name} (fnxf_xiii_budgeted_walk).\n${_fnxf_walk}")
set(_fnxf_walk_old "")
if(EXISTS "${_fnxf_walk_out}")
  file(READ "${_fnxf_walk_out}" _fnxf_walk_old)
endif()
if(NOT _fnxf_walk_old STREQUAL _fnxf_walk)
  file(WRITE "${_fnxf_walk_out}" "${_fnxf_walk}")
endif()
target_sources(fnxf_xiii PRIVATE "${_fnxf_walk_out}")
rexglue_setup_target(fnxf_xiii GPU_PLUGINS xenos)
# fnxf_xiii (2026-10-24): the linked Switch executable as out/switch/fnxf_xiii.elf (it had no extension); the NRO is made
# from it (rexglue_switch.cmake) and keeps its name, fnxf_xiii.nro.
set_target_properties(fnxf_xiii PROPERTIES SUFFIX ".elf")

if(TARGET rexcore)
    target_compile_definitions(rexcore PRIVATE REX_SWITCH_FALLBACK_EXE="/switch/fnxf_xiii/fnxf_xiii.nro")
endif()

# The renderer: reNut's Direct3D-level renderer on plume's deko3d back end.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fnxf_xiii_renut_gpu.cmake")

# The guest's float results must not change (no fused multiply-add it did not have), and the translated code reads
# the same memory as different types.
foreach(_t fnxf_xiii fnxf_xiii_recomp)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE -ffp-contract=off -fno-strict-aliasing)
    endif()
endforeach()
# fnxf_xiii (2026-11-01, diagnostic): build_switch.py --portable-fp: the instruction helpers of rex/ppc in their portable
# form (the expressions the code generator emitted before the AArch64 ones: as on x86, close to Xenia) instead of the
# AArch64 forms (vmx_fp.h's NEON paths, fpu_est.h's hardware tables, intrinsics.h's NEON helpers, context.h's record
# form). To tell whether one of those changes what the game does.
option(FNXF_XIII_PORTABLE_FP "Diagnostic: the portable forms of the PowerPC instruction helpers" OFF)
if(FNXF_XIII_PORTABLE_FP)
    foreach(_t fnxf_xiii fnxf_xiii_recomp)
        if(TARGET ${_t})
            target_compile_definitions(${_t} PRIVATE REX_VMX_FP_PORTABLE=1 REX_VMX_HELPERS_X86_FORM=1
                                                     REX_SETFROMMASK_MOVEMASK=1)
        endif()
    endforeach()
    message(STATUS "fnxf_xiii: FNXF_XIII_PORTABLE_FP: the portable instruction helpers (diagnostic)")
endif()

target_include_directories(fnxf_xiii PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${REXSDK_DIR}/src/core"
    "${REXSDK_DIR}/thirdparty/stb")  # fnxf_xiii (2026-10-25): stb_image for the language pack's glyphs
# fnxf_xiii (2026-10-23): native zlib for the game's decompression (src/cpu/fnxf_xiii_native_zlib.cpp).
target_link_libraries(fnxf_xiii PRIVATE "${DEVKITPRO}/portlibs/switch/lib/libz.a")
target_include_directories(fnxf_xiii PRIVATE "${DEVKITPRO}/portlibs/switch/include")
foreach(_dep xxhash)
    if(TARGET ${_dep})
        target_include_directories(fnxf_xiii PRIVATE $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# fnxf_xiii (2026-10-27): the NRO's icon, app/icon/icon.jpg (256x256 JPEG) when there is one (libnx's default
# otherwise). It is Square Enix's artwork: never part of the published kit (tools/make_kit.py leaves it out). The
# folder is a configure dependency, so an icon put there later is picked up (a changed one relinks: LINK_DEPENDS).
# fnxf_xiii (2026-11-08): app/icon/icon.png, when there is one, comes first: at build time scaled to 256x256 and saved as
# a JPEG of at most 100 kB (102,400 bytes) (tools/make_icon.py: the highest quality that fits; the Python standard library only)
# into the build folder; a changed PNG makes it again.
set(FNXF_XIII_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.jpg")
set(FNXF_XIII_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png")
set(FNXF_XIII_ICON_ARG)
if(EXISTS "${FNXF_XIII_ICON_PNG}")
    find_program(FNXF_XIII_PYTHON NAMES python3 python py NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(FNXF_XIII_ICON_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../tools/make_icon.py")
    set(FNXF_XIII_ICON "${CMAKE_CURRENT_BINARY_DIR}/icon/icon.jpg")
    add_custom_command(OUTPUT "${FNXF_XIII_ICON}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/icon"
        COMMAND "${FNXF_XIII_PYTHON}" "${FNXF_XIII_ICON_SCRIPT}" "${FNXF_XIII_ICON_PNG}" "${FNXF_XIII_ICON}" 256 102400
        DEPENDS "${FNXF_XIII_ICON_PNG}" "${FNXF_XIII_ICON_SCRIPT}"
        COMMENT "Making the NRO's icon from app/icon/icon.png"
        VERBATIM)
    add_custom_target(fnxf_xiii_icon DEPENDS "${FNXF_XIII_ICON}")
    add_dependencies(fnxf_xiii fnxf_xiii_icon)
    set(FNXF_XIII_ICON_ARG ICON "${FNXF_XIII_ICON}")
elseif(EXISTS "${FNXF_XIII_ICON}")
    set(FNXF_XIII_ICON_ARG ICON "${FNXF_XIII_ICON}")
endif()
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/icon")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icon")
endif()
# fnxf_xiii (2026-11-06): the port's own files in the NRO's romfs (src/fnxf_xiii_romfs.h): the repository's tex folder
# as romfs:/tex, and Renut's own shaders' DKSH (made with uam at build time: gpu.cmake renut_host_shader) as
# romfs:/shaders. Copied into a staging folder before each packaging; a changed or new file in tex relinks
# (LINK_DEPENDS, the folder a configure dependency), a changed shader is compiled again (a dependency of the target).
# Not game data: that never goes in the NRO (the game's shaders are translated and compiled on the console).
set(FNXF_XIII_TEX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tex")
set(FNXF_XIII_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
get_property(FNXF_XIII_ROMFS_SHADER_FILES GLOBAL PROPERTY FNXF_XIII_ROMFS_SHADERS)
set(FNXF_XIII_ROMFS_ARG)
if(IS_DIRECTORY "${FNXF_XIII_TEX_DIR}" OR FNXF_XIII_ROMFS_SHADER_FILES)
    set(FNXF_XIII_ROMFS_STAGE
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${FNXF_XIII_ROMFS_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${FNXF_XIII_ROMFS_DIR}")
    if(IS_DIRECTORY "${FNXF_XIII_TEX_DIR}")
        file(GLOB FNXF_XIII_TEX_FILES CONFIGURE_DEPENDS "${FNXF_XIII_TEX_DIR}/*")
        set_property(TARGET fnxf_xiii APPEND PROPERTY LINK_DEPENDS ${FNXF_XIII_TEX_FILES})
        list(APPEND FNXF_XIII_ROMFS_STAGE
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${FNXF_XIII_TEX_DIR}" "${FNXF_XIII_ROMFS_DIR}/tex")
    endif()
    if(FNXF_XIII_ROMFS_SHADER_FILES)
        list(APPEND FNXF_XIII_ROMFS_STAGE
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${FNXF_XIII_ROMFS_DIR}/shaders"
            COMMAND "${CMAKE_COMMAND}" -E copy ${FNXF_XIII_ROMFS_SHADER_FILES} "${FNXF_XIII_ROMFS_DIR}/shaders")
    else()
        message(STATUS "fnxf_xiii: no FNXF_XIII_UAM (the PC uam, build_switch.py): Renut's shaders are compiled on the console")
    endif()
    add_custom_command(TARGET fnxf_xiii POST_BUILD ${FNXF_XIII_ROMFS_STAGE}
        COMMENT "Staging the NRO's romfs (tex, shaders)"
        VERBATIM)
    set(FNXF_XIII_ROMFS_ARG ROMFS "${FNXF_XIII_ROMFS_DIR}")
endif()
rexglue_switch_add_nro(fnxf_xiii
    NAME "FNXF XIII"
    AUTHOR "recomp-nx"
    VERSION "1.0.1"
    ${FNXF_XIII_ICON_ARG}
    ${FNXF_XIII_ROMFS_ARG})

# fnxf_xiii (2026-10-26): FNXF_XIII_PGO=use: no "profile count data file not found" for code the profiling build never
# had in the game (the linker's --gc-sections dropped it: no profile written), the warning kept for everything else (a
# profile name that does not match shows there). zstd's and xxHash's copies of the shader recompiler, tiny-AES, plume's
# frame generation stub.
if(FNXF_XIII_PGO STREQUAL "use")
    foreach(_unused_target renut_zstd renut_xxhash tiny-aes)
        if(TARGET ${_unused_target})
            target_compile_options(${_unused_target} PRIVATE -Wno-missing-profile)
        endif()
    endforeach()
    if(TARGET plume)
        set_property(SOURCE "${CMAKE_SOURCE_DIR}/thirdparty/plume/plume_lsfg_stub.cpp"
                     DIRECTORY "${CMAKE_SOURCE_DIR}/thirdparty/plume" APPEND PROPERTY COMPILE_OPTIONS -Wno-missing-profile)
    endif()
endif()

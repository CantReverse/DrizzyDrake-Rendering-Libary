# Finds the Windows SDK shader compilers and provides drizzy_compile_shader(), which compiles HLSL into a C header at
# build time. The library embeds the bytecode, so it never compiles shaders at runtime.
#   fxc: DXBC (shader model 5.x) for Direct3D 11
#   dxc: DXIL (shader model 6.x) for Direct3D 12; dxil.dll next to dxc.exe signs the output

set(_drizzy_pf86 "ProgramFiles(x86)")
set(_drizzy_kits_bin "$ENV{${_drizzy_pf86}}/Windows Kits/10/bin")
file(GLOB _drizzy_sdk_dirs LIST_DIRECTORIES true "${_drizzy_kits_bin}/10.*")
list(SORT _drizzy_sdk_dirs COMPARE NATURAL ORDER DESCENDING)
set(_drizzy_hints "")
foreach(_dir IN LISTS _drizzy_sdk_dirs)
  list(APPEND _drizzy_hints "${_dir}/x64")
endforeach()
list(APPEND _drizzy_hints "${_drizzy_kits_bin}/x64")

if(DRIZZY_BUILD_D3D11)
  find_program(DRIZZY_FXC fxc HINTS ${_drizzy_hints} REQUIRED)
endif()
if(DRIZZY_BUILD_D3D12)
  find_program(DRIZZY_DXC dxc HINTS ${_drizzy_hints} REQUIRED)
endif()

# drizzy_compile_shader(SOURCE <hlsl> ENTRY <fn> PROFILE <vs_5_0|ps_6_0|...> VARIABLE <name> OUTPUT <header>)
# Profiles below 6.0 use fxc, 6.0 and above use dxc.
function(drizzy_compile_shader)
  cmake_parse_arguments(ARG "" "SOURCE;ENTRY;PROFILE;VARIABLE;OUTPUT" "DEPENDS" ${ARGN})
  get_filename_component(_src "${ARG_SOURCE}" ABSOLUTE)
  if(ARG_PROFILE MATCHES "_[6-9]_")
    set(_compiler "${DRIZZY_DXC}")
    set(_flags -nologo -O3 -Qstrip_reflect -Qstrip_debug -WX)
  else()
    set(_compiler "${DRIZZY_FXC}")
    set(_flags /nologo /O3 /Qstrip_reflect /Qstrip_debug /WX)
  endif()
  add_custom_command(
    OUTPUT "${ARG_OUTPUT}"
    COMMAND "${_compiler}" ${_flags} -T ${ARG_PROFILE} -E ${ARG_ENTRY} -Vn ${ARG_VARIABLE} -Fh "${ARG_OUTPUT}" "${_src}"
    DEPENDS "${_src}" ${ARG_DEPENDS}
    COMMENT "Compiling ${ARG_ENTRY} (${ARG_PROFILE})"
    VERBATIM)
endfunction()

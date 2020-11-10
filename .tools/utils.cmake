
function(print_variables)
  get_cmake_property(L_names VARIABLES)
  foreach (L_variable ${L_names})
    message(STATUS "[VARIABLE] ${L_variable} = ${${L_variable}}")
  endforeach()
endfunction()


function(build_asm files outputs)
  foreach (source_file ${files})
    get_filename_component(filename ${source_file} NAME)
    set(output_file "${filename}.txt")
    add_custom_command(
      OUTPUT ${output_file}
      COMMAND ${CMAKE_ASM_MASM_COMPILER} /Zi /c /Cp /Fl /Fo "${output_file}" "${source_file}"
    )
    list(APPEND ${outputs} ${output_file})
  endforeach()
endfunction()


macro(get_WIN32_WINNT version)
    if (CMAKE_SYSTEM_VERSION)
        set(ver ${CMAKE_SYSTEM_VERSION})
        string(REGEX MATCH "^([0-9]+).([0-9])" ver ${ver})
        string(REGEX MATCH "^([0-9]+)" verMajor ${ver})
        # Check for Windows 10, b/c we'll need to convert to hex 'A'.
        if ("${verMajor}" MATCHES "10")
            set(verMajor "A")
            string(REGEX REPLACE "^([0-9]+)" ${verMajor} ver ${ver})
        endif ("${verMajor}" MATCHES "10")
        # Remove all remaining '.' characters.
        string(REPLACE "." "" ver ${ver})
        # Prepend each digit with a zero.
        string(REGEX REPLACE "([0-9A-Z])" "0\\1" ver ${ver})
        set(${version} "0x${ver}")
    endif(CMAKE_SYSTEM_VERSION)
endmacro(get_WIN32_WINNT)


function(getTargetCPUAddressMode ADDRESS_MODE)
  if(${CMAKE_CXX_COMPILER_ARCHITECTURE_ID} STREQUAL "x64")
    set(${ADDRESS_MODE} 64 PARENT_SCOPE)
  elseif(${CMAKE_CXX_COMPILER_ARCHITECTURE_ID} STREQUAL "X86")
    set(${ADDRESS_MODE} 32 PARENT_SCOPE)
  else()
    message(FATAL_ERROR "Unknown address mode of architecture ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
  endif()
endfunction()


macro(set_target_postbuild_copy target)
  cmake_parse_arguments(ARG "" "" "TO" ${ARGN})
  foreach(destination ${ARG_TO})
    add_custom_command(
      TARGET ${target}
      POST_BUILD
      WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
      COMMAND node copy-file
        --input "$<TARGET_FILE:${target}>"
        --destination "${destination}"
      COMMAND node copy-file
        --input "$<TARGET_PDB_FILE:${target}>"
        --destination "${destination}"
    )
  endforeach()
endmacro()

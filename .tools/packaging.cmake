
############################################
# Package archiving functions
############################################

function(package_npm_archive LIST)
  cmake_parse_arguments(ARG "" "NAME;VERSION;CONFIG;DIRECTORY;DESTINATION" "DEPENDS" ${ARGN})
  if(NOT ARG_VERSION)
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
      set(ARG_VERSION "${CMAKE_PROJECT_VERSION}")
    else()
      set(ARG_VERSION "${CMAKE_PROJECT_VERSION}-${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    endif()
  endif()
  if(NOT ARG_CONFIG) 
    set(ARG_CONFIG "$<CONFIG>")
  endif()

  set(outputArchive "${CMAKE_CURRENT_BINARY_DIR}/packaging.log")
  add_custom_command(
    OUTPUT ${outputArchive}
    DEPENDS ${ARG_DEPENDS}
    WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
    COMMAND node pack_npm_archive
      --name "${ARG_NAME}"
      --version "${ARG_VERSION}"
      --config "${ARG_CONFIG}"
      --source "${ARG_DIRECTORY}"
      --destination "${ARG_DESTINATION}"
    COMMAND echo "done" > ${outputArchive}
  )
  set(${LIST} ${${LIST}} ${outputArchive} PARENT_SCOPE)
endfunction()


function(package_zip_archive LIST packageTag sourceDir destinationDir)
  cmake_parse_arguments(ARG "" "NAME;VERSION;CONFIG;DIRECTORY;DESTINATION" "DEPENDS" ${ARGN})
  if(NOT ARG_VERSION)
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
      set(ARG_VERSION "${CMAKE_PROJECT_VERSION}")
    else()
      set(ARG_VERSION "${CMAKE_PROJECT_VERSION}-${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    endif()
  endif()
  if(NOT ARG_CONFIG) 
    set(ARG_CONFIG "$<CONFIG>")
  endif()

  set(outputArchive "${ARG_DESTINATION}/${ARG_NAME}.zip")
  add_custom_command(
    OUTPUT ${outputArchive}
    DEPENDS ${ARG_DEPENDS}
    COMMAND ${CMAKE_COMMAND} 
      -E tar c "${outputArchive}" 
      --format=zip
      -- "${ARG_SOURCE}/**"
  )
  set(${LIST} ${${LIST}} ${outputArchive} PARENT_SCOPE)
endfunction()


############################################
# Static Files packaging functions
############################################

function(find_directory_files_for_copy SRCFILES DESTFILES sourceDir destinationDir)
  file(GLOB files "${sourceDir}/*")
  foreach(filepath ${files})
    get_filename_component(filename ${filepath} NAME)
    if(IS_DIRECTORY ${filepath})
      find_directory_files_for_copy(${SRCFILES} ${DESTFILES} "${sourceDir}/${filename}" "${destinationDir}/${filename}")
    else()
      list(APPEND ${SRCFILES} "${sourceDir}/${filename}")
      list(APPEND ${DESTFILES} "${destinationDir}/${filename}")
    endif()
  endforeach()
  set(${SRCFILES} ${${SRCFILES}} PARENT_SCOPE)
  set(${DESTFILES} ${${DESTFILES}} PARENT_SCOPE)
endfunction()

function(append_static_directory_copy LIST)
  cmake_parse_arguments(ARG "" "FILTER;TO" "FROM;DEPENDS" ${ARGN})
  set(inputFiles)
  set(outputFiles)
  foreach(sourceDir ${ARG_FROM})
    if(IS_DIRECTORY ${sourceDir})
      find_directory_files_for_copy(inputFiles outputFiles ${sourceDir} ${ARG_TO})
    else()
      message(FATAL_ERROR "The static directory '${sourceDir}' is invalid")
    endif()
  endforeach()
 
  # Replace '|' by '^|' due to a CMake bug
  if(ARG_FILTER)
    string(REPLACE "|" "^|" ARG_FILTER ${ARG_FILTER})
  else()
    set(ARG_FILTER "")
  endif()

  add_custom_command(
    OUTPUT ${outputFiles}
    DEPENDS ${inputFiles} ${ARG_DEPENDS}
    WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
    COMMAND node copy-directory
      --filter ${ARG_FILTER}
      --sources ${ARG_FROM}
      --destination ${ARG_TO}
  )
 
  set(${LIST} ${${LIST}} ${outputFiles} PARENT_SCOPE)
endfunction()

function(append_static_file_copy LIST)
  cmake_parse_arguments(ARG "" "FROM;TO" "FILES;DEPENDS" ${ARGN})
  foreach(inputFile ${ARG_FILES})
  get_filename_component(filename ${inputFile} NAME)
    if(ARG_FROM)
      set(inputFile "${ARG_FROM}/${inputFile}")
    endif()
    set(outputFile "${ARG_TO}/${filename}")
    add_custom_command(
      OUTPUT ${outputFile}
      DEPENDS ${inputFile} ${ARG_DEPENDS}
      WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
      COMMAND node copy-file
        --input ${inputFile}
        --output ${outputFile}
    )
    list(APPEND ${LIST} ${outputFile})
  endforeach()
  set(${LIST} ${${LIST}} PARENT_SCOPE)
endfunction()

############################################
# Target Files packaging functions
############################################

function(predict_target_binary_filename RESULT target)
  get_target_property(targetType ${target} TYPE)
  get_target_property(targetSuffix ${target} SUFFIX)
  if(targetSuffix)
    set(${RESULT} "${target}${targetSuffix}" PARENT_SCOPE)
  elseif(targetType STREQUAL "EXECUTABLE")
    set(${RESULT} "${target}.exe" PARENT_SCOPE)
  else()
    set(${RESULT} "${target}.dll" PARENT_SCOPE)
  endif()
endfunction()

function(append_target_copy LIST)
  cmake_parse_arguments(ARG "" "TO" "TARGETS;DEPENDS" ${ARGN})
  foreach(target ${ARG_TARGETS})
  
    # Compute output file path
    predict_target_binary_filename(binaryFile ${target})
    set(outputFile ${ARG_TO}/${binaryFile})
    
    # Create copy command
    get_target_property(sourceDir ${target} RUNTIME_OUTPUT_DIRECTORY)
    add_custom_command(
      OUTPUT ${outputFile}
      DEPENDS ${target} ${ARG_DEPENDS}
      WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
      COMMAND node copy-file
        --input $<TARGET_PDB_FILE:${target}>
        --destination ${ARG_TO}
      COMMAND node copy-file
        --input $<TARGET_FILE:${target}>
        --output ${outputFile}
    )
    list(APPEND ${LIST} ${outputFile})
  endforeach()
  set(${LIST} ${${LIST}} PARENT_SCOPE)
endfunction()

function(append_target_signed_copy LIST)
  cmake_parse_arguments(ARG "" "TO" "TARGETS;DEPENDS" ${ARGN})
  if(DEVMODE)
    append_target_copy(commands ${LIST} ${ARGN})
  else()
    foreach(target ${ARG_TARGETS})

      # Compute output file path
      predict_target_binary_filename(binaryFile ${target})
      set(outputFile ${ARG_TO}/${binaryFile})
      
      # Create copy command
      get_target_property(sourceDir ${target} RUNTIME_OUTPUT_DIRECTORY)
      add_custom_command(
        OUTPUT ${outputFile}
        DEPENDS ${target} ${ARG_DEPENDS}
        WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
        COMMAND node copy-file
          --input $<TARGET_PDB_FILE:${target}>
          --destination ${ARG_TO}
        COMMAND node copy-sign-file
          --input $<TARGET_FILE:${target}>
          --output ${outputFile}
      )
      list(APPEND ${LIST} ${outputFile})
    endforeach()
  endif()
  set(${LIST} ${${LIST}} PARENT_SCOPE)
endfunction()

function(predict_target_linker_filename RESULT target)
  set(${RESULT} "${target}.lib" PARENT_SCOPE)
endfunction()

function(append_target_library_copy LIST target destionationDir)
  cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})

  # Compute output file path
  predict_target_linker_filename(linkerFile ${target})
  set(outputFile ${destionationDir}/${linkerFile})
  
  # Create copy command
  get_target_property(sourceDir ${target} RUNTIME_OUTPUT_DIRECTORY)
  add_custom_command(
    OUTPUT ${outputFile}
    DEPENDS ${target} ${ARG_DEPENDS}
    WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
    COMMAND node copy-file
      --input $<TARGET_LINKER_FILE:${target}>
      --output ${outputFile}
  )
  set(${LIST} ${${LIST}} ${outputFile} PARENT_SCOPE)
endfunction()

############################################
# NodeJS packaging functions
############################################
function(append_transpiled_typescript LIST)
  cmake_parse_arguments(ARG "" "TSCONFIG;SOURCE;DESTINATION" "DIRECTORIES;DEPENDS" ${ARGN})
  set(outputFile "${CMAKE_CURRENT_BINARY_DIR}/nodejs-build.log")

  append_group_sources(ts_sources FILTER "*.ts|*.tsx" DIRECTORIES ${ARG_DIRECTORIES})

  add_custom_command(
    OUTPUT ${outputFile}
    DEPENDS ${ARG_TSCONFIG} ${ts_sources} ${ARG_DEPENDS}
    WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
    COMMAND node make_transpiled_typescript
      --tsConfigFile ${ARG_TSCONFIG}
      --source ${ARG_SOURCE}
      --destination ${ARG_DESTINATION}
    COMMAND ${CMAKE_COMMAND} -E echo "done" > ${outputFile}
  )
  set(${LIST} ${${LIST}} ${outputFile} PARENT_SCOPE)
endfunction()

function(append_package_json LIST)
  cmake_parse_arguments(ARG "PRODUCTION;INSTALL" "NAME;SOURCE;DESTINATION" "DEPENDS" ${ARGN})
  set(outputFile "${ARG_DESTINATION}/package.json")
  add_custom_command(
    OUTPUT ${outputFile}
    DEPENDS ${ARG_SOURCE} ${ARG_DEPENDS}
    WORKING_DIRECTORY ${TOOLS_NODE_SCRIPT_DIR}
    COMMAND node make_package_json
      --name ${ARG_NAME}
      --version ${CMAKE_PROJECT_VERSION}
      --source ${ARG_SOURCE}
      --destination ${ARG_DESTINATION}
      --install ${ARG_INSTALL}
      --production ${ARG_PRODUCTION}
  )
  set(${LIST} ${${LIST}} ${outputFile} PARENT_SCOPE)
endfunction()

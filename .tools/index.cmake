
# Set main directories variables
set_property(GLOBAL PROPERTY USE_FOLDERS TRUE)
if(NOT DELIVERY_OUTPUT_DIR)
  set(DELIVERY_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/.delivery)
  message(STATUS "Use default delivery directory at '${DELIVERY_OUTPUT_DIR}'")
endif()
if(NOT PROJECT_DEV_DIR)
  set(PROJECT_DEV_DIR ${CMAKE_BINARY_DIR}/.build.dev)
  message(STATUS "Use default development directory at '${PROJECT_DEV_DIR}'")
endif()
get_filename_component(DELIVERY_OUTPUT_DIR ${DELIVERY_OUTPUT_DIR} ABSOLUTE)
get_filename_component(PROJECT_DEV_DIR ${PROJECT_DEV_DIR} ABSOLUTE)
set(TOOLS_NODE_SCRIPT_DIR ${CMAKE_CURRENT_LIST_DIR}/scripts)

# Check devmode activation
# > devmode ON: improve building time
# > devmode OFF: add signing & improve reliability of building process
if(NOT DEVMODE)
  set(DEVMODE OFF)
else()
  message(STATUS "DEVMODE ON: Will build in developer mode, copy target directly in package & remove signing")
endif()

# Install node scripts
set(NODE_TOOLS "${CMAKE_SOURCE_DIR}/node_modules/tools")
if(NOT EXISTS ${NODE_TOOLS})
execute_process(COMMAND cmd /c npm install WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# Add used cmake library
include(${CMAKE_CURRENT_LIST_DIR}/utils.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/packaging.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/NodeJS.cmake)

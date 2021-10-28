
set(NODE_TOOLS "${CMAKE_SOURCE_DIR}/node_modules/script.cmake")
if(NOT EXISTS ${NODE_TOOLS})
execute_process(COMMAND cmd /c npm install WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
include(${NODE_TOOLS}/index.cmake)

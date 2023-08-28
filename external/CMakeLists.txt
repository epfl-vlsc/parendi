

# add_custom_target(vltKahypar
#     COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/external/kahypar
#     COMMAND ${CMAKE_COMMAND}

# )
find_package(Boost 1.69 REQUIRED COMPONENTS program_options)

add_library(vltKahypar STATIC kahypar/lib/libkahypar.cc)

target_link_libraries(vltKahypar ${Boost_LIBRARIES})

set_target_properties(vltKahypar PROPERTIES
    PUBLIC_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/include/libkahypar.h
    CXX_STANDARD 17)

target_include_directories(vltKahypar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/include)

target_include_directories(vltKahypar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/)

target_include_directories(vltKahypar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/external_tools/googletest/googletest/include
    ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/external_tools/googletest/googletest/
    ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/external_tools/googletest/googlemock/include
    ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/external_tools/googletest/googlemock/
)
target_include_directories(vltKahypar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/external_tools/kahypar-shared-resourced/
)
target_include_directories(vltKahypar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/kahypar/external_tools/
)

set(KAHYPAR_VERSION_GIT_SHA1 02f50d2ced65b408ca56820bf6d18a7c47a8518c)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/kahypar/kahypar/git_revision.h.in ${PROJECT_BINARY_DIR}/kahypar/git_revision.h)
target_include_directories(vltKahypar PRIVATE ${PROJECT_BINARY_DIR})


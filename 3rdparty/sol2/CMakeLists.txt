add_library(pcsx2-sol2 INTERFACE)
target_include_directories(pcsx2-sol2 INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcsx2-sol2 INTERFACE lua::lua)
target_compile_definitions(pcsx2-sol2 INTERFACE
    SOL_ALL_SAFETIES_ON=1
    SOL_USING_CXX_LUA=0
    SOL_EXCEPTIONS_ALWAYS_UNSAFE=1
    SOL_PRINT_ERRORS=0
)
add_library(sol2::sol2 ALIAS pcsx2-sol2)

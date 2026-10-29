seekdb_apply_unity_inventory(ob_sql SEEKDB_SQL_UNITY)
seekdb_apply_standalone_inventory(ob_sql SEEKDB_SQL_STANDALONE)
seekdb_apply_standalone_inventory(ob_sql_extra SEEKDB_SQL_EXTRA)

ob_add_new_object_target(ob_sql ob_sql)
ob_add_new_object_target(ob_sql_extra ob_sql_extra)
target_link_libraries(ob_sql PUBLIC ob_base config)
# File format decoding only. The package uses ABI 0 and bundles a different RE2
# from gRPC. A C interface plus hidden archive symbols isolates both ABIs and
# prevents symbol interposition; all relational operators execute in seekdb.
add_library(seekdb_file_parquet SHARED engine/basic/file_scan_parquet.cpp)
# Keep the runtime decoder beside the executable for relocatable build bundles.
set_target_properties(seekdb_file_parquet PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/observer")
target_include_directories(seekdb_file_parquet PRIVATE
  ${PROJECT_SOURCE_DIR}/src ${DEP_DIR}/include ${DEP_DIR}/include/apache-arrow)
target_link_options(seekdb_file_parquet PRIVATE
  -static-libstdc++ -static-libgcc
  -Wl,--exclude-libs,ALL -Wl,-Bsymbolic -Wl,--no-undefined
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/engine/basic/file_scan_parquet.map)
target_link_libraries(seekdb_file_parquet PRIVATE -Wl,--start-group
  ${DEP_DIR}/lib64/libparquet.a
  ${DEP_DIR}/lib64/libarrow.a
  ${DEP_DIR}/lib64/libarrow_bundled_dependencies.a
  ${DEP_DIR}/lib64/libz.a
  -Wl,--end-group pthread dl)
add_dependencies(ob_sql seekdb_file_parquet)
# The decoder is loaded only after engine initialization, on the first Parquet
# request. This avoids third-party static constructors running before seekdb's
# allocator is initialized and keeps disabled file SQL out of the startup path.
set_property(SOURCE engine/basic/file_scan_parquet_loader.cpp PROPERTY
  COMPILE_DEFINITIONS "FILE_SQL_PARQUET_LIBRARY=\"$<TARGET_FILE:seekdb_file_parquet>\"")
install(TARGETS seekdb_file_parquet LIBRARY DESTINATION usr/lib COMPONENT server)
target_link_libraries(ob_sql_extra PUBLIC ob_base_without_pass config)

seekdb_apply_unity_inventory(ob_sql_simd SEEKDB_SQL_SIMD_UNITY)
ob_server_add_target(ob_sql_simd)
if(ARCHITECTURE STREQUAL "x86_64" OR ARCHITECTURE STREQUAL "amd64")
  target_compile_options(ob_sql_simd PRIVATE
    -mtune=core-avx2 -mavx2 -mfma -mbmi2 -mavx512vl -mavx512bw)
endif()

if(WIN32)
  find_program(PWSH_EXE NAMES pwsh pwsh.exe powershell powershell.exe REQUIRED)
  execute_process(
    COMMAND ${PWSH_EXE} -NoProfile -ExecutionPolicy Bypass -File gen_parser_win.ps1
    RESULT_VARIABLE RET
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/src/sql/parser
    OUTPUT_FILE _gen_parser.output
    ERROR_FILE _gen_parser.error)
else()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env NEED_PARSER_CACHE=${NEED_PARSER_CACHE} bash gen_parser.sh
    RESULT_VARIABLE RET
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/src/sql/parser
    OUTPUT_FILE _gen_parser.output
    ERROR_FILE _gen_parser.error)
endif()
if(RET AND NOT RET EQUAL 0)
  message(FATAL_ERROR
    "Generate sql parser failed; inspect src/sql/parser/_gen_parser.error")
endif()
add_subdirectory(parser)

add_library(ob_sql_static STATIC EXCLUDE_FROM_ALL)
target_link_libraries(ob_sql_static PUBLIC
  ob_sql ob_sql_extra ob_sql_server_parser_static)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

add_clang_tool(clang-query
  ClangQuery.cpp

  GENERATE_DRIVER
  )
clang_target_link_libraries(clang-query
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangDynamicASTMatchers
  clangFrontend
  clangSerialization
  clangTooling
  clangQuery
  )

set(LLVM_LINK_COMPONENTS Support)

include_directories("../lib") # FIXME: use public APIs instead.
# Disable PCH reuse due to Clang issue where cl::opt<std::string> template
# instantiation is incomplete.
add_clang_tool(clang-include-cleaner IncludeCleaner.cpp DISABLE_PCH_REUSE GENERATE_DRIVER)
clang_target_link_libraries(clang-include-cleaner PRIVATE
  clangBasic
  clangFrontend
  clangLex
  clangFormat
  clangSerialization
  clangTooling
  clangIncludeCleaner
  )


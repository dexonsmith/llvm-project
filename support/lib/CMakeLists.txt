include(LLVM-Build)

add_subdirectory(Demangle)
add_subdirectory(Support)
add_subdirectory(TargetParser)
if (LLVM_INCLUDE_TESTS)
  add_subdirectory(Testing)
endif()

# Component post-processing
LLVMBuildResolveComponentsLink()
LLVMBuildGenerateCFragment(OUTPUT ${LLVMCONFIGLIBRARYDEPENDENCIESINC})

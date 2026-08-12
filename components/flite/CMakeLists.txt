file(GLOB_RECURSE sources ./**/*.cpp ./**/*.c)

idf_component_register(SRCS "${sources}" INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-implicit-fallthrough -Wno-error=implicit-fallthrough -Wno-unused-variable -Wno-unused-function)

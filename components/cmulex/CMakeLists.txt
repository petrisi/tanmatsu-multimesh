file(GLOB_RECURSE sources ./**.cpp ./**.c)
list(FILTER sources EXCLUDE REGEX "lex_data_include*")

idf_component_register(SRCS "${sources}" 
                       INCLUDE_DIRS "include" "lex_data_include"
                       REQUIRES flite)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-implicit-fallthrough -Wno-error=implicit-fallthrough -Wno-unused-variable -Wno-unused-function)

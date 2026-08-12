file(GLOB_RECURSE sources ./**.cpp ./**.c)
idf_component_register(SRCS "cmu_us_kal.c" "cmu_us_kal_diphone.c" 
                       INCLUDE_DIRS "."
                       REQUIRES flite cmulex usenglish)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-implicit-fallthrough -Wno-error=implicit-fallthrough -Wno-unused-variable -Wno-unused-function)

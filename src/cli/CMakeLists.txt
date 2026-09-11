add_library(onvifsim-cli-lib STATIC
    CliOptions.cpp
)
target_link_libraries(onvifsim-cli-lib PUBLIC onvifsim-core)
onvifsim_set_warnings(onvifsim-cli-lib)

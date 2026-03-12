otr_module(tracker-synthetic-b SUBDIRS ../tracker-synthetic)
target_include_directories(opentrack-tracker-synthetic-b PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(opentrack-tracker-synthetic-b PRIVATE SYNTHETIC_IS_B)

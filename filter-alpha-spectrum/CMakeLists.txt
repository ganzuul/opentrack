otr_module(filter-alpha-spectrum)

option(OPENTRACK_ALPHA_SPECTRUM_FETCH_IMNODES "Fetch Dear ImGui + imnodes for optional advanced node UI" OFF)

if(OPENTRACK_ALPHA_SPECTRUM_FETCH_IMNODES)
	include(FetchContent)

	FetchContent_Declare(
		imgui
		GIT_REPOSITORY https://github.com/ocornut/imgui.git
		GIT_TAG        v1.90.8-docking
	)

	FetchContent_Declare(
		imnodes
		GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
		GIT_TAG        v0.5
	)

	FetchContent_MakeAvailable(imgui imnodes)

	add_library(opentrack-alpha-spectrum-imgui STATIC
		${imgui_SOURCE_DIR}/imgui.cpp
		${imgui_SOURCE_DIR}/imgui_draw.cpp
		${imgui_SOURCE_DIR}/imgui_tables.cpp
		${imgui_SOURCE_DIR}/imgui_widgets.cpp
	)
	target_include_directories(opentrack-alpha-spectrum-imgui PUBLIC
		${imgui_SOURCE_DIR}
	)
	set_target_properties(opentrack-alpha-spectrum-imgui PROPERTIES
		PROJECT_LABEL ${self}-imgui
		FOLDER "${self}/dependencies"
	)

	add_library(opentrack-alpha-spectrum-imnodes STATIC
		${imnodes_SOURCE_DIR}/imnodes.cpp
	)
	target_compile_definitions(opentrack-alpha-spectrum-imnodes PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
	target_include_directories(opentrack-alpha-spectrum-imnodes PUBLIC
		${imgui_SOURCE_DIR}
		${imnodes_SOURCE_DIR}
	)
	target_link_libraries(opentrack-alpha-spectrum-imnodes PUBLIC opentrack-alpha-spectrum-imgui)
	set_target_properties(opentrack-alpha-spectrum-imnodes PROPERTIES
		PROJECT_LABEL ${self}-imnodes
		FOLDER "${self}/dependencies"
	)

	target_compile_definitions(${self} PRIVATE HAS_IMNODES=1)
	target_link_libraries(${self} PRIVATE opentrack-alpha-spectrum-imnodes)
else()
	target_compile_definitions(${self} PRIVATE HAS_IMNODES=0)
endif()

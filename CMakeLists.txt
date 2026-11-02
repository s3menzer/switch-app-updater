cmake_minimum_required(VERSION 3.13)

add_library(switch-updater STATIC
	updater.cpp
	manifest.cpp
	third_party/json-parser/json.c
)

target_include_directories(switch-updater PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
)

# The Switch launcher belongs to the updater project. The parent application
# supplies the launcher source to its NRO target so the generated
# the launcher NRO remains in the top-level build directory.
set(SWITCH_UPDATER_LAUNCHER_SOURCE
	"${CMAKE_CURRENT_SOURCE_DIR}/launcher.cpp"
	PARENT_SCOPE
)

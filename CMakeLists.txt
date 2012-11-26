project(JUBJUBD C)
cmake_minimum_required(VERSION 2.6)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_GLIB2 glib-2.0)
pkg_check_modules(PC_GIO2 gio-2.0)
pkg_check_modules(PC_GIOUNIX2 gio-unix-2.0)

include_directories(${PC_GLIB2_INCLUDE_DIRS} ${PC_GIO2_INCLUDE_DIRS} ${PC_GIOUNIX2_INCLUDE_DIRS})

add_executable(jubjubd jubjubd.c jubjub-gen.c)

target_link_libraries(jubjubd ${PC_GLIB2_LIBRARIES} ${PC_GIO2_LIBRARIES} ${PC_GIOUNIX2_LIBRARIES})

add_custom_command(OUTPUT jubjub-gen.c jubjub-gen.h MAIN_DEPENDENCY jubjub.xml COMMAND gdbus-codegen jubjub.xml --generate-c-code jubjub-gen --interface-prefix pl.koriakin.Jubjub --c-namespace Jubjub)

install(TARGETS jubjubd
	RUNTIME DESTINATION bin
	LIBRARY DESTINATION lib${LIB_SUFFIX}
	ARCHIVE DESTINATION lib${LIB_SUFFIX})

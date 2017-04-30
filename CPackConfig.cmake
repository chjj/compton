# == Environment ==
if (NOT CPACK_SYSTEM_NAME)
	set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_PROCESSOR}")
	if (CPACK_SYSTEM_NAME STREQUAL "x86_64")
		set(CPACK_SYSTEM_NAME "amd64")
	endif ()
endif ()

# == Basic information ==
set(CPACK_PACKAGE_NAME "compton")
set(CPACK_PACKAGE_VENDOR "chjj")
set(CPACK_PACKAGE_VERSION "${COMPTON_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION "A lightweight X compositing window manager, fork of xcompmgr-dana.")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight X compositing window manager")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}")
set(CPACK_PACKAGE_CONTACT "nobody <devnull@example.com>")

# == Package config ==
set(CPACK_INSTALLED_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/build" "usr")
set(CPACK_GENERATOR "TBZ2" "DEB" "RPM")
set(CPACK_RESOURCE_FILE_LICENSE "LICENSE")
set(CPACK_RESOURCE_FILE_README "README.md")
set(CPACK_STRIP_FILES 1)

# == DEB package config ==
set(CPACK_DEBIAN_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}${PATCH_VERSION}~git${GIT_DATE}")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CPACK_SYSTEM_NAME}")
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.15), libconfig9, libdbus-1-3 (>= 1.1.1), libgl1-mesa-glx | libgl1 | libgl1-nvidia-glx | libgl1-fglrx-glx, libpcre3 (>= 8.10), libx11-6, libxcomposite1 (>= 1:0.3-1), libxdamage1 (>= 1:1.1), libxext6, libxfixes3, libxrandr2 (>= 4.3), libxrender1, libxinerama1")

# == RPM package config ==
set(CPACK_RPM_PACKAGE_REQUIRES "/bin/sh,libGL.so.1,libX11.so.6,libXcomposite.so.1,libXdamage.so.1,libXext.so.6,libXfixes.so.3,libXrandr.so.2,libXrender.so.1,libc.so.6,libconfig.so.9,libdbus-1.so.3,libm.so.6,libpcre.so.1")

# == Source package config ==
set(CPACK_SOURCE_GENERATOR "TBZ2 DEB RPM")

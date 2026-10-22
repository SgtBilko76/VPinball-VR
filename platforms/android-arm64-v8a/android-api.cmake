# Forces the Android API level of every dependency build started by external.sh
# (see the comment there). The toolchain file is read by project(), after the
# set(CMAKE_SYSTEM_VERSION ...) calls of the dependency CMakeLists.txt files.
if(DEFINED ENV{VPX_ANDROID_API})
   set(CMAKE_SYSTEM_VERSION $ENV{VPX_ANDROID_API})
endif()

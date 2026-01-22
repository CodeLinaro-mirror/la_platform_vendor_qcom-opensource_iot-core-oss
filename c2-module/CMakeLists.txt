cmake_minimum_required(VERSION 3.8.2)
project(IOT_CORE_OSS
  VERSION ${IOT_CORE_OSS_VERSION}
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)

# GStreamer plugin.
set(GST_QTI_C2_MODULE qtic2module)

add_library(${GST_QTI_C2_MODULE} SHARED
  c2-module.cc
)

set_target_properties(${GST_QTI_C2_MODULE} PROPERTIES PUBLIC_HEADER
  c2-module.h
)

math(EXPR VERSION_MAJOR "${GST_CODEC2_CONFIG_VERSION_MAJOR} + 0")
math(EXPR VERSION_MINOR "${GST_CODEC2_CONFIG_VERSION_MINOR} + 0")

target_compile_definitions(${GST_QTI_C2_MODULE} PRIVATE
  CODEC2_CONFIG_VERSION_MAJOR=${VERSION_MAJOR}
  CODEC2_CONFIG_VERSION_MINOR=${VERSION_MINOR}
  _LINUX_
  $<$<BOOL:${GST_ENABLE_AUDIO_PLUGINS}>:ENABLE_AUDIO_PLUGINS>
  # This is a WA due to mismatch of _LINUX_ and __LINUX__ defines
  # in audio and video parts of the Codec2.
  # Refer to this:
  # codec2/codecs/include/QC2Config.h#28
  # mm-audio-headers/codec2/codecs/include/QC2Config.h#14
  $<$<BOOL:${GST_ENABLE_AUDIO_PLUGINS}>:__LINUX__>
)

target_link_libraries(${GST_QTI_C2_MODULE} PRIVATE
  codec2_vndk
  $<$<BOOL:${GST_ENABLE_AUDIO_PLUGINS}>:qc2audio_utils>
)

install(
  TARGETS ${GST_QTI_C2_MODULE}
  LIBRARY DESTINATION ${IOT_CORE_OSS_INSTALL_LIBDIR}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
  PUBLIC_HEADER DESTINATION ${IOT_CORE_OSS_INSTALL_INCDIR}
)


cmake_minimum_required(VERSION 3.24...3.25)

legacy_check()

add_library(channel-copier MODULE)
add_library(OBS::channel-copier ALIAS channel-copier)

target_sources(channel-copier PRIVATE channel-copier.cpp)

target_link_libraries(
 channel-copier
  PRIVATE
    OBS::libobs
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in channel-copier.rc)
  target_sources(channel-copier PRIVATE channel-copier.rc)
endif()

set_target_properties_obs(channel-copier PROPERTIES FOLDER plugins PREFIX "")

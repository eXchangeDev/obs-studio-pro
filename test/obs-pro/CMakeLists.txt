find_package(nlohmann_json 3.11 REQUIRED)

function(add_obs_pro_session_test target source)
  add_executable(${target})
  target_sources(
    ${target}
    PRIVATE
      ${source}
      ../../frontend/utility/MultitrackConfigProvider.cpp
      ../../frontend/utility/OutputRoute.cpp
      ../../frontend/utility/PlatformSession.cpp
  )
  target_include_directories(${target} PRIVATE ../../frontend)
  target_link_libraries(${target} PRIVATE OBS::libobs nlohmann_json::nlohmann_json)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(
    ${target}
    PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>"
  )
  set_target_properties(${target} PROPERTIES FOLDER "tests and examples")
endfunction()

add_obs_pro_session_test(obs-pro-platform-session-model-test PlatformSessionModelTest.cpp)
add_obs_pro_session_test(obs-pro-platform-session-lifecycle-test PlatformSessionLifecycleTest.cpp)

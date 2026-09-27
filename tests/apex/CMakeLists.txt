# apex 纯函数单测：不依赖 httplib / sqlite / llama。文件名遵守 names.md（无 _/-）。

add_executable(apexverdict verdict.cpp)
target_compile_features(apexverdict PRIVATE cxx_std_17)
add_test(NAME apexverdict COMMAND apexverdict)
set_tests_properties(apexverdict PROPERTIES LABELS "apex;unit" TIMEOUT 30)

add_executable(apexdecide decide.cpp)
target_compile_features(apexdecide PRIVATE cxx_std_17)
add_test(NAME apexdecide COMMAND apexdecide)
set_tests_properties(apexdecide PROPERTIES LABELS "apex;unit" TIMEOUT 30)

# The Windows set's files as fetch-driver.sh writes them, from check-lock.sh's lines: `files_var`
# holds the driver and the notices, the libraries follow, and each renamed file is named by its
# rename row's embedded name. The fetch's outputs are then the files it writes, so the build does
# not fetch again each time. sources.cmake calls it; embed-build-graph_test.sh runs it with
# cmake -P.
function(redshift_windows_set_files files_var platform gen_dir lock_lines)
  set(files ${${files_var}})
  foreach(line IN LISTS lock_lines)
    if(line MATCHES "^library ${platform} ([^ ]+) ")
      list(APPEND files "${gen_dir}/${CMAKE_MATCH_1}")
    endif()
  endforeach()
  foreach(line IN LISTS lock_lines)
    if(line MATCHES "^rename ${platform} ([^ ]+) ([^ ]+)$")
      set(original "${gen_dir}/${CMAKE_MATCH_1}")
      set(embedded "${gen_dir}/${CMAKE_MATCH_2}")
      list(FIND files "${original}" at)
      if(at GREATER -1)
        list(REMOVE_AT files ${at})
        list(INSERT files ${at} "${embedded}")
      endif()
    endif()
  endforeach()
  set(${files_var} "${files}" PARENT_SCOPE)
endfunction()

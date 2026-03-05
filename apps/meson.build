prognames = ['test_fs']
foreach p : prognames
  executable(p + '.x', p + '.c',
    include_directories : fs_inc,
    link_with: fs_lib)
endforeach

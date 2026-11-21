# cmake-format settings for this repository's CMake files. DuckDB's format check (make
# format-check) runs cmake-format on the CMakeLists.txt files, the root one included. The lines
# here follow the repository's 120-column C++ style, and comments are left as written, so their
# lists and wrapping survive formatting.
with section("format"):
    line_width = 120

with section("markup"):
    enable_markup = False

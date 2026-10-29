# Writes build_stamp.h with the time of this build, for the log's first line (which NRO ran). Run at every build.
string(TIMESTAMP now "%Y-%m-%d %H:%M")
file(WRITE "${OUT}.tmp" "#define STREET_NX_STAMP \"${now}\"\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)

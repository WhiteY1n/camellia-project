savedcmd_mouse_input_filter.mod := printf '%s\n'   mouse_input_filter.o | awk '!x[$$0]++ { print("./"$$0) }' > mouse_input_filter.mod

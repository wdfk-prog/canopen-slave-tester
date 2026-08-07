# Stop at main after the dynamic loader has mapped dependent libraries.
# Load every Lely shared library for which matching local symbols are available.

set breakpoint pending on

tbreak main
commands
  silent
  sharedlibrary .*liblely-.*
  printf "Attempted to load all Lely shared-library symbols.\n"
  info sharedlibrary
end

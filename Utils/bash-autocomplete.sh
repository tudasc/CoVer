# Please add "source /path/to/bash-autocomplete.sh" to your .bashrc to use this.

_@EXECUTABLE_WRAPPER_NAME@_filedir()
{
  # _filedir function provided by recent versions of bash-completion package is
  # better than "compgen -f" because the former honors spaces in pathnames while
  # the latter doesn't. So we use compgen only when _filedir is not provided.
  _filedir 2> /dev/null || COMPREPLY=( $( compgen -f ) )
}

_@EXECUTABLE_WRAPPER_NAME@()
{
  local cur prev words cword arg flags w1 w2
  # If latest bash-completion is not supported just initialize COMPREPLY and
  # initialize variables by setting manually.
  _init_completion -n 2> /dev/null
  if [[ "$?" != 0 ]]; then
    COMPREPLY=()
    cword=$COMP_CWORD
    cur="${COMP_WORDS[$cword]}"
  fi

  w1="${COMP_WORDS[$cword - 1]}"
  if [[ $cword > 1 ]]; then
    w2="${COMP_WORDS[$cword - 2]}"
  fi

  # Pass all the current command-line flags to @EXECUTABLE_WRAPPER_NAME@, so that @EXECUTABLE_WRAPPER_NAME@ can handle
  # these internally.
  # '=' is separated differently by bash, so we have to concat them without ','
  for i in `seq 1 $cword`; do
    if [[ $i == $cword || "${COMP_WORDS[$(($i+1))]}" == '=' ]]; then
      arg="$arg${COMP_WORDS[$i]}"
    else
      arg="$arg${COMP_WORDS[$i]},"
    fi
  done

  # expand ~ to $HOME
  eval local path=${COMP_WORDS[0]}
  # Use $'\t' so that bash expands the \t for older versions of sed.
  flags=$( "$path" --autocomplete="$arg" 2>/dev/null | sed -e $'s/\t.*//' )
  # If @EXECUTABLE_WRAPPER_NAME@ is old that it does not support --autocomplete,
  # fall back to the filename completion.
  if [[ "$?" != 0 ]]; then
    _@EXECUTABLE_WRAPPER_NAME@_filedir
    return
  fi

  # When @EXECUTABLE_WRAPPER_NAME@ does not emit any possible autocompletion, or user pushed tab after " ",
  # just autocomplete files.
  if [[ "$flags" == "$(echo -e '\n')" ]]; then
    # If -foo=<tab> and there was no possible values, autocomplete files.
    [[ "$cur" == '=' || "$cur" == -*= ]] && cur=""
    _@EXECUTABLE_WRAPPER_NAME@_filedir
  elif [[ "$cur" == '=' ]]; then
    COMPREPLY=( $( compgen -W "$flags" -- "") )
  else
    # Bash automatically appends a space after '=' by default.
    # Disable it so that it works nicely for options in the form of -foo=bar.
    [[ "${flags: -1}" == '=' ]] && compopt -o nospace 2> /dev/null
    COMPREPLY=( $( compgen -W "$flags" -- "$cur" ) )
  fi
}
complete -F _@EXECUTABLE_WRAPPER_NAME@ @EXECUTABLE_WRAPPER_NAME@

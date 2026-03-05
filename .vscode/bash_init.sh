if [ -f ~/.bashrc ]; then
    source ~/.bashrc
fi

WORKSPACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPLETIONS_DIR="${WORKSPACE_ROOT}/install/share/bash-completion/completions"

if [ -d "$COMPLETIONS_DIR" ]; then
    # Enable nullglob so if no .sh files exist, the loop skips instead of crashing
    shopt -s nullglob
    
    loaded_count=0
    for completion_file in "$COMPLETIONS_DIR"/*.sh; do
        if [ -f "$completion_file" ]; then
            source "$completion_file"
            ((loaded_count++))
        fi
    done
    
    # Turn nullglob back off to behave normally for the rest of your session
    shopt -u nullglob
fi

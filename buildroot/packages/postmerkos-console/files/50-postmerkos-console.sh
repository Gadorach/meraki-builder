# Launch postmerkOS management for interactive SSH/login shells belonging to a
# configured administrator, operator, or viewer. Serial direct-entry is handled
# by postmerkos-serial-login rather than this profile.
case "$-" in *i*) ;; *) return 0 2>/dev/null || exit 0 ;; esac
[ -t 0 ] && [ -t 1 ] || return 0 2>/dev/null || exit 0
[ "${POSTMERKOS_RAW_SHELL:-0}" = 1 ] && return 0 2>/dev/null || true
[ "${POSTMERKOS_NO_CONSOLE:-${POSTMERKOS_NO_CLI:-0}}" = 1 ] && return 0 2>/dev/null || true

if ! command -v postmerkosctl >/dev/null 2>&1; then
    [ "$(id -u 2>/dev/null)" = 0 ] && {
        echo 'WARNING: postmerkOS management tools are unavailable; entering the root recovery shell.'
        return 0 2>/dev/null || exit 0
    }
    echo 'postmerkOS management tools are unavailable.' >&2
    exit 1
fi

session_error=$(mktemp /tmp/postmerkos-session.XXXXXX 2>/dev/null || echo /tmp/postmerkos-session.$$)
if session=$(postmerkosctl session --shell 2>"$session_error"); then
    POSTMERKOS_USERNAME= POSTMERKOS_ROLE=none POSTMERKOS_CAPABILITIES=
    case "$session" in
      *POSTMERKOS_USERNAME=*POSTMERKOS_ROLE=*POSTMERKOS_CAPABILITIES=*) eval "$session" ;;
      *) POSTMERKOS_ROLE=none ;;
    esac
    rm -f "$session_error"
    if [ "${POSTMERKOS_ROLE:-none}" = none ]; then
        echo 'This account has no postmerkOS management role.' >&2
        exit 1
    fi
else
    detail=$(cat "$session_error" 2>/dev/null || true)
    rm -f "$session_error"
    if [ "$(id -u 2>/dev/null)" = 0 ]; then
        echo 'WARNING: the postmerkOS management service is unavailable; entering the root recovery shell.'
        [ -n "$detail" ] && echo "Reason: $detail"
        return 0 2>/dev/null || exit 0
    fi
    echo 'The postmerkOS management service is unavailable.' >&2
    [ -n "$detail" ] && echo "Reason: $detail" >&2
    exit 1
fi

postmerkos-console menu
exit $?

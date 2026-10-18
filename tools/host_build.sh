# host_build.sh — sourced by the host suites, never run: compile each source
# to its own object, through ccache when it is installed, then link.
#
# A suite that hands every source to one `cc` recompiles all of them on every
# run, and ccache cannot cache a compile of many files at once. The mutant
# harnesses run a suite once per mutant, each in a new copy of the tree, so
# most of a mutant's time went to recompiling files it had not touched. One
# object per source makes each cacheable, and CCACHE_BASEDIR makes ccache
# read paths inside the tree as relative ones, so every copy shares one
# cache: a mutant recompiles the file it changed.
#
#   . tools/host_build.sh            (after cd to the top of the tree)
#   host_flags=(...)                 compile flags
#   host_sources=(...)               sources, relative to the top
#   host_objects "$work"             fills host_objs, in host_sources' order
#   cc "${host_objs[@]}" ... -o ...  the link is the suite's own
#
# A suite whose sources need flags of their own calls host_compile per source
# and host_wait after the last.
#
# HOST_BUILD_JOBS caps the compiles running at once (default: every CPU); a
# harness running twenty suites side by side sets it low. CCACHE_DISABLE=1
# compiles without the cache, as it does for the makefiles.

if command -v ccache > /dev/null 2>&1; then
    host_cc=(ccache cc)
else
    host_cc=(cc)
fi
export CCACHE_BASEDIR="$PWD"
# Debug info names the directory a compile ran in, so ccache keys a -g
# compile on it unless told not to; a cache keyed on the copy's own path is
# a cache no other copy can hit.
export CCACHE_NOHASHDIR=1

# One compile, in the background, with any flags this source alone needs
# after the shared ones; it waits for a free slot first. host_wait collects
# them all and fails if any did.
host_pids=()
host_build_failed=0
host_compile() {
    local src=$1 obj=$2
    shift 2
    if [ "${#host_pids[@]}" -ge "${HOST_BUILD_JOBS:-$(nproc)}" ]; then
        wait "${host_pids[0]}" || host_build_failed=1
        host_pids=("${host_pids[@]:1}")
    fi
    "${host_cc[@]}" "${host_flags[@]}" "$@" -c "$src" -o "$obj" &
    host_pids+=("$!")
}

host_wait() {
    local pid
    for pid in "${host_pids[@]}"; do
        wait "$pid" || host_build_failed=1
    done
    host_pids=()
    return "$host_build_failed"
}

host_objects() {
    local work=$1 src obj
    host_objs=()
    for src in "${host_sources[@]}"; do
        obj="$work/${src//\//_}.o"
        host_compile "$src" "$obj"
        host_objs+=("$obj")
    done
    host_wait
}

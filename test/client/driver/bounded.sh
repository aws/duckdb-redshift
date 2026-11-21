# bounded <command>...: runs it for at most $BOUNDED_SECONDS seconds, 20 unless the suite that
# sources this file sets another, so a case that would wait on a FIFO or loop on a bad payload fails
# instead of hanging the suite. coreutils' timeout, which would do this, is not on every macOS host,
# and the tools need none (lib/download.sh), so the suites do not either. As that timeout does, it
# stops the command's whole process group, since what blocks may be a child the command forked; the
# kill is in a finally block, so an interrupted run (SIGTERM, SIGHUP or SIGINT, which exit 128 plus
# the signal's number, or any other exception) leaves nothing of it behind. Only the first of those
# signals counts, and none once the finally block has begun, so a later one cannot skip the kill; one
# that comes while the command is being started ends the run once it has started, so it is killed
# too. Sourced by the suites that run a tool or a child that could hang; common_test.sh tests it.
# shellcheck shell=bash

: "${BOUNDED_SECONDS:=20}"
bounded() {
  BOUNDED_SECONDS="$BOUNDED_SECONDS" python3 -I -c 'import os, signal, subprocess, sys
seconds = float(os.environ["BOUNDED_SECONDS"])
stops = (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)
p = None
caught = []
def stop(n, _):
    if caught:
        return  # a signal before this one, or the finally block, decides
    caught.append(n)
    if p is not None:  # else the command may be starting: exit once it has, below, so that it is killed
        sys.exit(128 + n)
# Without a handler TERM and HUP end Python at once and skip the finally block below.
for s in stops:
    signal.signal(s, stop)
try:
    p = subprocess.Popen(sys.argv[1:], start_new_session=True)
    if caught:
        sys.exit(128 + caught[0])
    sys.exit(p.wait(timeout=seconds))
except subprocess.TimeoutExpired:
    sys.stderr.write("timed out after %g seconds\n" % seconds)
    sys.exit(124)
finally:
    caught.append(None)  # from here on a signal changes nothing
    # Still running: the wait timed out, or something interrupted it.
    if p is not None and p.poll() is None:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()' "$@"
}

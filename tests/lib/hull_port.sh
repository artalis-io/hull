# hull_port.sh - listening ports for a POSIX harness, without the free-port race.
#
# WHY THIS EXISTS. The usual trick - bind port 0, read the port, close, then
# hand the number to the real listener - leaves a gap: between the close and the
# listener's bind, any other process on the machine may take the port. On a CI
# runner that runs jobs side by side it does: e2e_ssh_tunnel's relay shim died
# with "Address already in use" on Linux aarch64. Two ways out, by listener:
#
#   - A listener we write (the Python shims and fake peers) binds port 0 itself
#     and reports the port it got in its ready file: no gap at all. Read it with
#     hull_port_ready_file.
#   - A listener that takes a fixed port (sshd, hull -p) cannot report one, so
#     the gap stays; hull_port_retry closes it by starting the listener again on
#     a new port when the bind fails. The start function must tell a failed bind
#     apart from success by the LISTENER's own output (its "listening" log
#     line) and its process staying alive - never by a connect alone, which a
#     foreign process holding the port would also answer.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# A port the kernel says is free right now (it may not be by the time it is
# used; only hull_port_retry should call this).
hull_port_free() {
    python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}

# Wait for FILE to hold a port number (written by a listener that bound port 0,
# after listen(), by rename so it is never seen half-written) and print it.
# Returns 1 after ~10 s without one.
hull_port_ready_file() {
    _hp_i=0
    while :; do
        if [ -s "$1" ]; then
            _hp_port=$(head -n 1 "$1" | tr -dc '0-9')
            if [ -n "$_hp_port" ]; then echo "$_hp_port"; return 0; fi
        fi
        _hp_i=$((_hp_i + 1))
        [ "$_hp_i" -gt 100 ] && return 1
        sleep 0.1 2>/dev/null || sleep 1
    done
}

# hull_port_retry TRIES START_FN: call START_FN PORT with a fresh free port
# until it returns 0 (listening) or TRIES attempts returned 2 (the bind failed:
# try another port). Any other status is returned at once. On success
# HULL_PORT holds the port.
hull_port_retry() {
    _hp_tries=$1; _hp_fn=$2
    _hp_n=0
    while [ "$_hp_n" -lt "$_hp_tries" ]; do
        HULL_PORT=$(hull_port_free)
        _hp_rc=0
        "$_hp_fn" "$HULL_PORT" || _hp_rc=$?
        [ "$_hp_rc" -ne 2 ] && return "$_hp_rc"
        _hp_n=$((_hp_n + 1))
    done
    return 2
}

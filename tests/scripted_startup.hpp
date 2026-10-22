#pragma once

#include <string>

namespace obd::test {

// Inline fixture prologues: the existing script remains the supervised process
// and keeps fd 3. No proxy or buffered reader may change PID/EOF choreography.
// These peers have no kernel obligations and become ready only after the grant.
inline std::string shell_startup_handshake(int device_id) {
    return R"SH(_obd_startup_generation=
_obd_startup_next=false
for _obd_startup_arg in "$@"; do
    if [ "$_obd_startup_next" = true ]; then
        _obd_startup_generation=$_obd_startup_arg
        _obd_startup_next=false
    elif [ "$_obd_startup_arg" = --startup-generation ]; then
        _obd_startup_next=true
    fi
done
case "$_obd_startup_generation" in
    ''|0|*[!0-9]*) exit 2 ;;
esac
printf '{"state":"starting","startup_owner":{"version":1,"generation":%s,"revision":0,"phase":"preparing","device_id":-1,"reason_errno":0,"control_pending":false,"kernel_debt_known":true,"raw_published":0,"raw_retired":0,"release_sequence":0,"release":"none","release_errno":0}}\n' "$_obd_startup_generation" >&3
# The supervisor emits compact JSON with lexically ordered object keys.
_obd_startup_expected='{"cmd":"startup_begin","generation":'"$_obd_startup_generation"',"version":1}'
IFS= read -r _obd_startup_command <&3 || exit 1
[ "$_obd_startup_command" = "$_obd_startup_expected" ] || exit 2
printf '{"state":"ready","device":"/dev/ublkb)SH" +
        std::to_string(device_id) +
        R"SH(","startup_owner":{"version":1,"generation":%s,"revision":2,"phase":"ready","device_id":)SH" +
        std::to_string(device_id) +
        R"SH(,"reason_errno":0,"control_pending":false,"kernel_debt_known":true,"raw_published":0,"raw_retired":0,"release_sequence":0,"release":"none","release_errno":0}}\n' "$_obd_startup_generation" >&3
)SH";
}

inline std::string python_startup_handshake(int device_id) {
    return R"PY(import json, os, sys
_obd_generation = int(sys.argv[sys.argv.index('--startup-generation') + 1])
if not 0 < _obd_generation < 2**64:
    raise SystemExit(2)
_obd_owner = dict(version=1, generation=_obd_generation, revision=0,
                 phase='preparing', device_id=-1, reason_errno=0,
                 control_pending=False, kernel_debt_known=True,
                 raw_published=0, raw_retired=0, release_sequence=0,
                 release='none', release_errno=0)
def _obd_report(value):
    data = memoryview((json.dumps(value) + '\n').encode())
    while data:
        count = os.write(3, data)
        if count <= 0:
            raise SystemExit(1)
        data = data[count:]
_obd_report(dict(state='starting', startup_owner=_obd_owner))
_obd_line = bytearray()
while not _obd_line.endswith(b'\n'):
    value = os.read(3, 1)
    if not value:
        raise SystemExit(1)
    _obd_line.extend(value)
if json.loads(_obd_line) != dict(cmd='startup_begin',
                               generation=_obd_generation, version=1):
    raise SystemExit(2)
_obd_owner.update(phase='ready', revision=2, device_id=)PY" +
        std::to_string(device_id) +
        R"PY()
_obd_report(dict(state='ready', device='/dev/ublkb)PY" +
        std::to_string(device_id) +
        R"PY(', startup_owner=_obd_owner))
)PY";
}

} // namespace obd::test

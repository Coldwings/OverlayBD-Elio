#!/usr/bin/env python3
"""Interleave two existing builds; retain every trial and never kill an owner."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import time


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def save(path, value):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def compilers():
    result = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            comm = (entry / 'comm').read_text().strip()
            command = (entry / 'cmdline').read_bytes().split(b'\0')
        except OSError:
            continue
        if comm in {'cc1plus', 'cc1', 'clang', 'clang++'} or (
                comm == 'cmake' and b'--build' in command):
            result.append({'pid': int(entry.name), 'comm': comm})
    return result


def git(source, *arguments):
    return os.fsdecode(subprocess.check_output(
        ['git', '-c', 'safe.directory=*', '-C', str(source), *arguments]))


def include_roots(entry):
    arguments = entry.get('arguments') or shlex.split(entry['command'])
    roots, index = [], 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in {'-I', '-isystem'}:
            index += 1
            roots.append((Path(entry['directory']) / arguments[index]).resolve())
        elif argument.startswith('-I'):
            roots.append((Path(entry['directory']) / argument[2:]).resolve())
        index += 1
    return roots


def native_elio(entry):
    # Identify the header implementation actually selected by this compile
    # command, including a FetchContent source override. A project pin alone
    # does not identify an existing binary's header-only implementation.
    for include in include_roots(entry):
        marker = include / 'elio/runtime/async_main.hpp'
        if not marker.is_file():
            continue
        marker = marker.resolve()
        header_root, package = marker.parents[1], marker.parents[3]
        headers = {str(path): sha(path) for path in sorted(header_root.rglob('*'))
                   if path.is_file() and path.suffix in {'.hpp', '.h', '.ipp', '.tpp'}}
        repository = None
        if (package / '.git').exists():
            repository = {'root': str(package), 'observed_head': git(package, 'rev-parse', 'HEAD').strip(),
                          'status_z': git(package, 'status', '--porcelain=v1', '-z')}
        cmake = package / 'CMakeLists.txt'
        return {'observed_header_root': str(header_root), 'header_hashes': headers,
                'observed_package_git': repository,
                'package_cmake_sha256': sha(cmake) if cmake.is_file() else None}
    raise RuntimeError('cannot identify native Elio include root from benchmark compile command')


def cache_value(cache, name):
    return next(line.split('=', 1)[1] for line in cache.splitlines()
                if line.split(':', 1)[0] == name and '=' in line)


def link_details(build, cache, binary):
    generator = cache_value(cache, 'CMAKE_GENERATOR')
    if generator == 'Unix Makefiles':
        path = build / 'benchmarks/CMakeFiles/obd-bench-ublk-read.dir/link.txt'
        command, cwd = path.read_text(), build / 'benchmarks'
        origin = {'file': str(path)}
    elif generator == 'Ninja':
        query = [cache_value(cache, 'CMAKE_MAKE_PROGRAM'), '-C', str(build),
                 '-t', 'commands', 'obd-bench-ublk-read']
        commands = os.fsdecode(subprocess.check_output(query)).splitlines()
        selected = []
        for command_line in commands:
            arguments = shlex.split(command_line)
            for index, argument in enumerate(arguments[:-1]):
                if argument == '-o' and (build / arguments[index+1]).resolve() == binary:
                    selected.append(command_line)
                    break
        if len(selected) != 1:
            raise RuntimeError('cannot identify unique Ninja benchmark link command')
        command, cwd = selected[0], build
        origin = {'query': query, 'selected_command': command}
    else:
        raise RuntimeError('unsupported benchmark generator: ' + generator)
    libraries = [(cwd / item).resolve() for item in shlex.split(command)
                 if not item.startswith('-') and
                 (item.endswith('.a') or re.search(r'\.so(?:\.[0-9]+)*$', item))]
    return {'generator': generator, 'cwd': str(cwd), 'command': command,
            'origin': origin, 'libraries': libraries}


def describe(build, allow_dirty, require_artifacts=True):
    build = build.resolve()
    cache = build / 'CMakeCache.txt'
    source_line = next(line for line in cache.read_text().splitlines()
                       if line.startswith('CMAKE_HOME_DIRECTORY:INTERNAL='))
    source = Path(source_line.split('=', 1)[1])
    binary = build / 'benchmarks/obd-bench-ublk-read'
    status = git(source, 'status', '--porcelain=v1', '-z')
    if status and not allow_dirty:
        raise RuntimeError(f'{source} is dirty; use a committed tree or --allow-dirty')
    link = link_details(build, cache.read_text(), binary)
    entries = json.loads((build / 'compile_commands.json').read_text())
    entry = next(item for item in entries
                 if Path(item['file']).name == 'ublk_read.cpp')
    selected = {'ublk_read.cpp', 'queue.cpp', 'device.cpp', 'elio_bridge.cpp',
                'local_file.cpp', 'lsmt.cpp', 'obd_mkimage_main.cpp'}
    def artifact(path):
        if require_artifacts or path.is_file():
            return sha(path)
        return None
    return {'build': str(build), 'source': str(source),
            'provenance_kind': 'observed checkout and artifacts; not verified binary build inputs',
            'observed_checkout_head': git(source, 'rev-parse', 'HEAD').strip(),
            'observed_checkout_status_z': status,
            'observed_dirty_patch': git(source, 'diff', '--binary', 'HEAD'),
            'binary': str(binary), 'binary_sha256': artifact(binary),
            'writer_sha256': artifact(build / 'tools/obd-mkimage'),
            'cache_sha256': sha(cache), 'cache': cache.read_text(),
            'compile_commands_sha256': sha(build / 'compile_commands.json'),
            'relevant_compile_commands': [item for item in entries if Path(item['file']).name in selected],
            'observed_native_elio': native_elio(entry),
            'link_command': {key: value for key, value in link.items() if key != 'libraries'},
            'libraries': {str(path): artifact(path) for path in link['libraries']},
            'observed_tracked_source_hashes': {name: sha(source / name)
                for name in git(source, 'ls-files', '-z').split('\0')
                if name and (source / name).is_file()}}


def input_identity(record):
    # Fields expected to remain fixed through the public build and matrix;
    # binary/archive hashes are instead frozen after the optional build.
    keys = ['source', 'observed_checkout_head', 'observed_checkout_status_z',
            'observed_dirty_patch', 'observed_tracked_source_hashes',
            'cache_sha256', 'compile_commands_sha256', 'relevant_compile_commands',
            'observed_native_elio']
    return {key: record[key] for key in keys}


def verify_frozen(binding, allow_dirty):
    current = describe(Path(binding['build']), allow_dirty)
    if current != binding:
        raise RuntimeError('observed source/cache/native headers/artifacts changed; stop matrix')


def cpu_metadata():
    models = set()
    for line in Path('/proc/cpuinfo').read_text().splitlines():
        key, separator, value = line.partition(':')
        if separator and key.strip() in {'model name', 'Hardware', 'Processor'}:
            models.add(value.strip())
    return {'model_labels': sorted(models), 'cpu_count': os.cpu_count(),
            'runner_affinity': sorted(os.sched_getaffinity(0)),
            'runner_status': Path('/proc/self/status').read_text()}


def measure(prefix):
    fio = json.loads(prefix.with_suffix('.fio.json').read_text())
    warm = json.loads(prefix.with_suffix('.warm.json').read_text())
    server = json.loads(prefix.with_suffix('.server.json').read_text())
    job = fio['jobs'][0]
    if (job['error'] or warm['jobs'][0]['error'] or not server['cleanup_complete']
            or server['measured']['exit'] or server['warm']['exit']):
        raise RuntimeError('fio or server reported a failure')
    ios = job['read']['total_ios']
    if not ios:
        raise RuntimeError('fio completed no reads')
    before, after = server['before'], server['after']
    user = (after['user_us'] - before['user_us']) / ios
    system = (after['system_us'] - before['system_us']) / ios
    def ticks(stat):
        return sum(map(int, stat[stat.rfind(')') + 2:].split()[11:13]))
    return {'ios': ios, 'iops': job['read']['iops'],
            'fio_mean_clat_us': job['read']['clat_ns']['mean'] / 1000,
            'fio_mean_lat_us': job['read']['lat_ns']['mean'] / 1000,
            'clat_percentiles_ns': job['read']['clat_ns']['percentile'],
            'cpu_user_us_per_io': user, 'cpu_system_us_per_io': system,
            'cpu_us_per_io': user + system,
            'proc_cpu_us_per_io': (ticks(after['process_stat']) - ticks(before['process_stat']))
                / before['clock_ticks'] * 1e6 / ios,
            'switches_per_io': (after['voluntary'] + after['involuntary']
                - before['voluntary'] - before['involuntary']) / ios,
            'achieved_iodepth': job['iodepth_level']}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, required=True)
    parser.add_argument('--candidate-build', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='new retained result directory')
    parser.add_argument('--workers', type=int, required=True)
    parser.add_argument('--blocking-threads', type=int, default=2)
    parser.add_argument('--queues', type=int, default=1)
    parser.add_argument('--queue-depth', type=int, default=128)
    parser.add_argument('--qds', type=int, nargs='+', default=[1, 16])
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--bytes', type=int, default=64 << 20)
    parser.add_argument('--warm-seconds', type=int, default=1)
    parser.add_argument('--run-seconds', type=int, default=5)
    parser.add_argument('--observe-seconds', type=float, default=120)
    parser.add_argument('--fio', default='fio')
    parser.add_argument('--sudo', action='store_true', help='explicitly launch only the server via sudo -n')
    parser.add_argument('--allow-dirty', action='store_true')
    parser.add_argument('--rebuild-before-run', action='store_true',
                        help='public incremental CMake build of bench+mkimage before measurement')
    args = parser.parse_args(argv)
    for value in [args.workers, args.blocking_threads, args.queues, args.queue_depth,
                  args.repeats, args.bytes, args.warm_seconds, args.run_seconds,
                  args.observe_seconds, *args.qds]:
        if value <= 0:
            parser.error('numeric settings must be positive')
    if len(args.qds) != len(set(args.qds)):
        parser.error('duplicate QDs would overwrite trial files')
    if not math.isfinite(args.observe_seconds):
        parser.error('--observe-seconds must be finite')
    if args.bytes % 4096:
        parser.error('--bytes must be a multiple of4096')
    return args


def reserve_output(out, variants):
    if any(Path(binding['source']) in [out, *out.parents] for binding in variants.values()):
        raise RuntimeError('output must be outside both source checkouts')
    out.mkdir(parents=True, exist_ok=False)


def verify_files(expected):
    for path, digest in expected.items():
        if sha(path) != digest:
            raise RuntimeError('fixed input changed: ' + path)


def observe_process(process, record, results_path, records, seconds):
    record.update(pid=process.pid, state='running', compiler_observations=[])
    try:
        save(results_path, records)
        start = time.monotonic()
        while process.poll() is None:
            found = compilers()
            if found:
                record['compiler_observations'].append(found)
            if time.monotonic() - start >= seconds:
                raise TimeoutError('observation timeout; owner retained')
            time.sleep(0.1)
    except BaseException:
        record['state'] = 'interrupted_or_timed_out_owner_retained'
        save(results_path, records)
        raise
    record.update(state='exited', exit=process.returncode)
    save(results_path, records)


def main():
    args = parse_args()
    if compilers():
        raise RuntimeError('compiler active; coordinate a quiet window before running')
    fio = shutil.which(args.fio)
    if not fio:
        raise RuntimeError('fio executable not found')
    fio = str(Path(fio).resolve())
    variants = {name: describe(build, args.allow_dirty, not args.rebuild_before_run)
                for name, build in [('baseline', args.baseline_build), ('candidate', args.candidate_build)]}
    out = args.out.resolve()
    reserve_output(out, variants)
    build_steps = []
    save(out / 'observations-before-build.json', variants)
    if args.rebuild_before_run:
        for name, binding in list(variants.items()):
            if compilers():
                raise RuntimeError('another compiler active before optional rebuild')
            command = ['cmake', '--build', binding['build'], '--target',
                       'obd-bench-ublk-read', 'obd-mkimage', '--parallel', '1']
            with (out / (name + '.build.log')).open('w') as log:
                code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
            step = {'variant': name, 'command': command, 'exit': code,
                    'kind': 'public incremental build, not a clean rebuild or cryptographic proof'}
            build_steps.append(step)
            save(out / 'build-steps.json', build_steps)
            if code:
                raise RuntimeError('optional rebuild failed; no matrix started')
            after = describe(Path(binding['build']), args.allow_dirty)
            if input_identity(after) != input_identity(binding):
                raise RuntimeError('source/cache/HEAD/native headers changed during build; no matrix started')
            variants[name] = after
    # Freeze actual post-build artifacts; rebuilding is never done in a trial.
    for binding in variants.values():
        verify_frozen(binding, args.allow_dirty)
    writer = args.baseline_build.resolve() / 'tools/obd-mkimage'
    # Dense ordinary data is written rather than truncate()/sparse extents.
    chunk = b''.join(hashlib.sha256(str(n).encode('ascii')).digest() for n in range(32768))
    raw = out / 'fixture.raw'
    with raw.open('wb') as stream:
        remaining = args.bytes
        while remaining:
            portion = chunk[:min(remaining, len(chunk))]
            stream.write(portion)
            remaining -= len(portion)
    writer_command = [str(writer), '--input', str(raw), '--out-dir', str(out), '--name', 'fixture']
    with (out / 'writer.stdout').open('w') as stdout, (out / 'writer.stderr').open('w') as stderr:
        subprocess.run(writer_command, stdout=stdout, stderr=stderr, check=True)
    image = out / 'fixture.lsmt'
    # Prewarm by reading: this is NOT a residency measurement or guarantee.
    with image.open('rb') as stream:
        while stream.read(1 << 20):
            pass
    fixed_inputs = {str(path): sha(path) for path in [raw, image, Path(fio)]}
    settings = vars(args).copy()
    settings.update(baseline_build=str(args.baseline_build), candidate_build=str(args.candidate_build), out=str(out))
    provenance = {'variants': variants, 'settings': settings, 'fixed_inputs': fixed_inputs,
        'kernel': platform.release(), 'uname': list(platform.uname()),
        'cpu': cpu_metadata(), 'loadavg': os.getloadavg(),
        'optional_build_steps': build_steps,
        'binary_input_claim': 'Observed stable inputs/artifacts; optional incremental build recorded; no generic cryptographic build proof.',
        'cache_state': 'prewarmed by read; residency not verified',
        'fio': fio, 'fio_sha256': sha(fio),
        'fio_version': subprocess.check_output([fio, '--version'], text=True).strip(),
        'writer_command': writer_command, 'writer_sha256': sha(writer),
        'raw_sha256': sha(raw), 'image_sha256': sha(image), 'image_bytes': image.stat().st_size,
        'runner_sha256': sha(Path(__file__)),
        'cpu_accounting': 'RUSAGE_SELF user+system / same measured fio IO count; excludes fio children; includes invocation/snapshot dispatch overhead.'}
    save(out / 'provenance.json', provenance)
    records = []
    for repeat in range(args.repeats):
        for mode in ['zero', 'lsmt']:
            for qd in args.qds:
                order = ['baseline', 'candidate'] if repeat % 2 == 0 else ['candidate', 'baseline']
                for variant in order:
                    if compilers():
                        raise RuntimeError('compiler started; matrix stopped before admitting another device')
                    binding = variants[variant]
                    verify_frozen(binding, args.allow_dirty)
                    verify_files(fixed_inputs)
                    if compilers():
                        raise RuntimeError('compiler started during provenance checks')
                    prefix = out / f'{mode}-q{qd}-r{repeat+1}-{variant}'
                    config = {'mode': mode, 'image': str(image), 'prefix': str(prefix), 'fio': fio,
                              'bytes': args.bytes, 'workers': args.workers, 'blocking_threads': args.blocking_threads,
                              'queues': args.queues, 'queue_depth': args.queue_depth, 'qd': qd,
                              'warm_seconds': args.warm_seconds, 'run_seconds': args.run_seconds}
                    save(prefix.with_suffix('.config.json'), config)
                    command = (['sudo', '-n'] if args.sudo else []) + [binding['binary'], '--config', str(prefix)+'.config.json']
                    record = {'mode': mode, 'qd': qd, 'repeat': repeat+1, 'variant': variant,
                              'command': command, 'state': 'starting', 'loadavg': os.getloadavg()}
                    records.append(record)
                    with prefix.with_suffix('.log').open('w') as log:
                        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                        observe_process(process, record, out / 'results.json', records,
                                        args.observe_seconds)
                    try:
                        verify_files(fixed_inputs)
                    except Exception as error:
                        record['fixed_input_error'] = str(error)
                        save(out / 'results.json', records)
                        raise
                    if process.returncode or record['compiler_observations']:
                        raise RuntimeError('failed or contaminated trial; inspect retained ownership/logs; no next trial')
                    record.update(measure(prefix))
                    record['state'] = 'complete'
                    save(out / 'results.json', records)
    summary = []
    for mode in ['zero', 'lsmt']:
        for qd in args.qds:
            for variant in variants:
                trials = [r for r in records if (r['mode'], r['qd'], r['variant']) == (mode, qd, variant)]
                metrics = ['iops', 'fio_mean_clat_us', 'fio_mean_lat_us', 'cpu_us_per_io', 'switches_per_io']
                summary.append({'mode': mode, 'qd': qd, 'variant': variant,
                    'medians': {key: statistics.median(r[key] for r in trials) for key in metrics}})
    for binding in variants.values():
        verify_frozen(binding, args.allow_dirty)
    verify_files(fixed_inputs)
    save(out / 'summary.json', summary)
    save(out / 'artifact-hashes.json', {str(p.relative_to(out)): sha(p)
        for p in out.iterdir() if p.is_file() and p.name != 'artifact-hashes.json'})
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()

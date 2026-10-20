#!/usr/bin/env python3
"""Unprivileged runner/metadata controls; no Device or kernel IO is modeled."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('benchmark_runner', ROOT / 'benchmarks/run_ublk_read.py')
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class RunnerControls(unittest.TestCase):
    def test_duplicate_and_nonfinite_rejected_before_admission(self):
        base = ['--baseline-build', 'baseline', '--candidate-build', 'candidate',
                '--out', 'results', '--workers', '1']
        for options in [['--qds', '1', '1'], ['--observe-seconds', 'nan'],
                        ['--observe-seconds', 'inf'], ['--observe-seconds=-inf']]:
            with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    RUNNER.parse_args(base + options)
                self.assertEqual(failure.exception.code, 2)

    def test_output_reservation_never_overwrites_existing_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / 'result'
            out.mkdir()
            sentinel = out / 'retain'
            sentinel.write_text('existing experiment')
            with self.assertRaises(FileExistsError):
                RUNNER.reserve_output(out, {})
            self.assertEqual(sentinel.read_text(), 'existing experiment')
            with self.assertRaisesRegex(RuntimeError, 'outside'):
                RUNNER.reserve_output(out / 'nested', {'baseline': {'source': str(out)}})
            self.assertFalse((out / 'nested').exists())

    def test_output_reservation_resolves_source_and_output_aliases(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            source = home / 'source'
            source.mkdir()
            alias = home / 'source-alias'
            alias.symlink_to(source, target_is_directory=True)
            for out, checkout in [(source / 'source-alias-result', alias),
                                  (alias / 'output-alias-result', source)]:
                with self.subTest(out=out, checkout=checkout):
                    with self.assertRaisesRegex(RuntimeError, 'outside'):
                        RUNNER.reserve_output(out, {'baseline': {'source': str(checkout)}})
                    self.assertFalse(out.exists())

    def test_fixed_input_changes_are_detected_for_each_member(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = [Path(temporary) / name for name in ['raw', 'image', 'fio']]
            for path in paths:
                path.write_bytes(b'original')
            expected = {str(path): RUNNER.sha(path) for path in paths}
            RUNNER.verify_files(expected)
            for path in paths:
                with self.subTest(path=path):
                    path.write_bytes(b'changed')
                    with self.assertRaisesRegex(RuntimeError, 'fixed input changed'):
                        RUNNER.verify_files(expected)
                    path.write_bytes(b'original')

    def test_ninja_query_selects_link_and_uses_build_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary).resolve()
            binary = build / 'benchmarks/obd-bench-ublk-read'
            cache = 'CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_MAKE_PROGRAM:FILEPATH=/selected/ninja\n'
            commands = ('c++ -o benchmarks/CMakeFiles/bench.dir/file.cpp.o -c source.cpp\n'
                        ': && c++ file.o -o benchmarks/obd-bench-ublk-read '
                        'src/libobd_ublk.a /system/liburing.so && :\n')
            with mock.patch.object(RUNNER.subprocess, 'check_output', return_value=commands.encode()) as query:
                link = RUNNER.link_details(build, cache, binary)
            query.assert_called_once_with(['/selected/ninja', '-C', str(build), '-t',
                                           'commands', 'obd-bench-ublk-read'])
            self.assertEqual(link['cwd'], str(build))
            self.assertEqual(link['libraries'][0], build / 'src/libobd_ublk.a')
            self.assertEqual(link['libraries'][1], Path('/system/liburing.so'))
            with self.assertRaisesRegex(RuntimeError, 'unsupported'):
                RUNNER.link_details(build, 'CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\n', binary)

    def metadata_fixture(self, home):
        source, build, native = home / 'source', home / 'build', home / 'native'
        (source / 'benchmarks').mkdir(parents=True)
        (source / 'benchmarks/ublk_read.cpp').write_text('// metadata input, never compiled\n')
        (source / 'name\nwith-newline').write_text('NUL-delimited git enumeration')
        for command in [['init', '-q'], ['add', '--all'], ['-c', 'user.name=Runner Control',
                '-c', 'user.email=control@example.invalid', '-c', 'commit.gpgsign=false',
                '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'metadata fixture']]:
            subprocess.run(['git', '-C', str(source), *command], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
        header = native / 'include/elio/runtime/async_main.hpp'
        header.parent.mkdir(parents=True)
        header.write_text('// selected native header identity\n')
        (native / 'CMakeLists.txt').write_text('# native package metadata\n')
        (build / 'benchmarks/CMakeFiles/obd-bench-ublk-read.dir').mkdir(parents=True)
        (build / 'tools').mkdir()
        (build / 'src').mkdir()
        (build / 'benchmarks/obd-bench-ublk-read').write_bytes(b'observed driver artifact')
        (build / 'tools/obd-mkimage').write_bytes(b'observed writer artifact')
        (build / 'src/libcore.a').write_bytes(b'observed archive')
        (build / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + str(source) +
                                              '\nCMAKE_GENERATOR:INTERNAL=Unix Makefiles\n')
        entry = {'directory': str(build), 'file': str(source / 'benchmarks/ublk_read.cpp'),
                 'arguments': ['c++', '-I' + str(native / 'include'), '-c', str(source / 'benchmarks/ublk_read.cpp')]}
        (build / 'compile_commands.json').write_text(json.dumps([entry]))
        (build / 'benchmarks/CMakeFiles/obd-bench-ublk-read.dir/link.txt').write_text(
            'c++ bench.o -o obd-bench-ublk-read ../src/libcore.a\n')
        return source, build, header

    def test_observed_provenance_records_override_and_detects_changed_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, build, header = self.metadata_fixture(Path(temporary))
            binding = RUNNER.describe(build, False)
            self.assertIn('not verified binary build inputs', binding['provenance_kind'])
            self.assertIn('name\nwith-newline', binding['observed_tracked_source_hashes'])
            self.assertIn(str(header), binding['observed_native_elio']['header_hashes'])
            self.assertEqual(binding['relevant_compile_commands'][0]['arguments'][1],
                             '-I' + str(header.parents[2]))
            RUNNER.verify_frozen(binding, False)
            for path in [header, build / 'CMakeCache.txt', build / 'src/libcore.a',
                         build / 'benchmarks/obd-bench-ublk-read', source / 'name\nwith-newline']:
                original = path.read_bytes()
                with self.subTest(path=path):
                    path.write_bytes(original + b'change')
                    with self.assertRaises(RuntimeError):
                        RUNNER.verify_frozen(binding, True)
                    path.write_bytes(original)
            RUNNER.verify_frozen(binding, False)

    def test_observation_timeout_retains_real_child_until_cooperative_release(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            release = directory / 'release'
            # An ordinary finite child demonstrates observer ownership, not a
            # fake Device. No numeric PID signal or kill method is used.
            code = ('from pathlib import Path; import sys,time; '
                    'p=Path(sys.argv[1]); end=time.monotonic()+5\n'
                    'while not p.exists() and time.monotonic()<end: time.sleep(.01)\n')
            process = subprocess.Popen([sys.executable, '-c', code, str(release)], start_new_session=True)
            record, records = {}, []
            records.append(record)
            try:
                with mock.patch.object(RUNNER, 'compilers', return_value=[]):
                    with self.assertRaises(TimeoutError):
                        RUNNER.observe_process(process, record, directory / 'results.json', records, .05)
                self.assertIsNone(process.poll())
                saved = json.loads((directory / 'results.json').read_text())[0]
                self.assertEqual(saved['state'], 'interrupted_or_timed_out_owner_retained')
                self.assertEqual(saved['pid'], process.pid)
            finally:
                release.touch()
                self.assertEqual(process.wait(timeout=10), 0)


if __name__ == '__main__':
    unittest.main()

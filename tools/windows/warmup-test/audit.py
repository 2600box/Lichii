"""Fail the artifact build if core compiler commands lack PGO or ThinLTO."""
import json
from pathlib import Path

out = Path('evidence')
out.mkdir(exist_ok=True)
commands = json.loads(Path('build/release/compile_commands.json').read_text())
core = [c for c in commands if '/src/' in c['file'].replace('\\', '/')]
assert core, 'No project compile commands'
for entry in core:
    command = entry['command']
    assert 'clang-cl' in command, entry['file']
    assert '-fprofile-use=' in command and 'lichii-windows.profdata' in command, entry['file']
    assert '-flto=thin' in command, entry['file']
    assert '-fprofile-generate' not in command, entry['file']
required = ['vk_pipeline_library.cpp', 'vk_graphics_pipeline.cpp', 'shader_warm_builder.cpp',
            'warm_records.cpp', 'spirv_inline_control.cpp']
for name in required:
    assert any(c['file'].replace('\\', '/').endswith('/'+name) for c in core), name
log = (out/'build.log').read_text(errors='replace')
warnings = [line for line in log.splitlines() if any(s in line.lower() for s in
    ('warning:', 'profile data', 'hash mismatch', 'unprofiled', 'out of date'))
    and ('warning:' in line.lower() or 'mismatch' in line.lower())]
(out/'warnings.txt').write_text('\n'.join(warnings)+'\n')
(out/'optimisation-audit.txt').write_text(
    f'{len(core)} project compile commands use clang-cl, -flto=thin and the required -fprofile-use.\n'
    f'Warm-up translation units: {required}\n'
    'Training is never run. Existing profile reused; new/changed functions may be unprofiled.\n'
    'Profile warnings are demoted from errors, not suppressed. See warnings.txt and build.log.\n'
    'Tests enabled relative to official release. Room/Discord disabled as in official release.\n'
    'GUI smoke test is not gameplay or GPU/performance validation.\n')
print((out/'optimisation-audit.txt').read_text())

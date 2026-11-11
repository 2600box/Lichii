"""Fail the artifact build if core compiler commands lack PGO or ThinLTO."""
import json
from pathlib import Path
from collections import Counter
import re

out = Path('evidence')
out.mkdir(exist_ok=True)
commands = json.loads(Path('build/release/compile_commands.json').read_text())
core = [c for c in commands if '/src/' in c['file'].replace('\\', '/')]
core = [c for c in core if Path(c['file']).suffix in ('.c', '.cc', '.cpp', '.cxx')]
assert core, 'No project compile commands'
pgo_disabled = []
lto_disabled = []
for entry in core:
    command = entry['command']
    assert 'clang-cl' in command, entry['file']
    assert '-fprofile-use=' in command and 'lichii-windows.profdata' in command, entry['file']
    assert '-flto=thin' in command, entry['file']
    assert '-fprofile-generate' not in command, entry['file']
    if '-fno-profile-instr-use' in command:
        pgo_disabled.append(entry['file'])
    if '-fno-lto' in command:
        lto_disabled.append(entry['file'])
assert all(p.replace('\\', '/').endswith('/common/zbic_compression.cpp') for p in pgo_disabled), pgo_disabled
assert not lto_disabled, lto_disabled
required = ['vk_pipeline_library.cpp', 'vk_graphics_pipeline.cpp', 'shader_warm_builder.cpp',
            'warm_records.cpp', 'spirv_inline_control.cpp']
for name in required:
    assert any(c['file'].replace('\\', '/').endswith('/'+name) for c in core), name
log = (out/'build.log').read_text(errors='replace')
warnings = [line for line in log.splitlines() if 'warning:' in line.lower()]
hash_mismatches = [line for line in warnings if 'hash mismatch' in line]
files = Counter(re.search(r'warning: (.+?): function', line).group(1)
                for line in hash_mismatches if re.search(r'warning: (.+?): function', line))
(out/'profile-mismatch-summary.json').write_text(json.dumps({
    'hash_mismatch_warnings': len(hash_mismatches), 'by_file': dict(files),
    'pgo_disabled_sources': pgo_disabled, 'lto_disabled_sources': lto_disabled}, indent=2)+'\n')
(out/'warnings.txt').write_text('\n'.join(warnings)+'\n')
(out/'optimisation-audit.txt').write_text(
    f'{len(core)} project C/C++ commands contain clang-cl, -flto=thin and the required -fprofile-use.\n'
    f'Effective PGO opt-outs retained from baseline: {pgo_disabled}.\n'
    f'Effective ThinLTO opt-outs: {lto_disabled}.\n'
    f'Profile control-flow hash mismatch warnings: {len(hash_mismatches)}.\n'
    f'Warm-up translation units: {required}\n'
    'Training is never run. Existing profile reused; new/changed functions may be unprofiled.\n'
    'Profile warnings are demoted from errors, not suppressed. See warnings.txt and build.log.\n'
    'Tests enabled relative to official release. Room/Discord disabled as in official release.\n'
    'GUI smoke test is not gameplay or GPU/performance validation.\n')
print((out/'optimisation-audit.txt').read_text())

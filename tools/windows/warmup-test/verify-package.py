"""Check the delivered PE and portable layout without loading any game content."""
import hashlib
import json
import struct
from pathlib import Path

package = Path('package')
exe = package / 'lichii.exe'
data = exe.read_bytes()
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[:2] == b'MZ' and data[pe:pe+4] == b'PE\0\0'
assert struct.unpack_from('<H', data, pe+4)[0] == 0x8664, 'Not AMD64'
assert struct.unpack_from('<H', data, pe+24)[0] == 0x20b, 'Not PE32+'
markers = ['VK_EXT_graphics_pipeline_library', 'use_graphics_pipeline_library',
           'shader_calls_not_inlined', 'precache_shaders', 'shader_warm_threads',
           'Build Shader Precache']
for marker in markers:
    assert marker.encode() in data or marker.encode('utf-16le') in data, marker
user_files = [str(p.relative_to(package)) for p in (package/'user').rglob('*') if p.is_file()]
assert user_files == ['user/PORTABLE.txt'] or user_files == ['user\\PORTABLE.txt'], user_files
for forbidden in ('*.nsp', '*.xci', '*.nup', '*.nca', '*.nso', '*.keys', '*.profraw'):
    assert not list(package.rglob(forbidden)), forbidden
assert (package/'platforms/qwindows.dll').is_file()
for dll in ('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll'):
    assert (package/dll).is_file(), dll
Path('evidence').mkdir(exist_ok=True)
Path('evidence/binary-check.json').write_text(json.dumps({
    'machine': 'AMD64', 'format': 'PE32+',
    'exe_sha256': hashlib.sha256(data).hexdigest(),
    'compiled_feature_markers': markers, 'user_files': user_files,
    'note': 'Feature presence is not a GPU functionality or performance test.'}, indent=2)+'\n')
print(Path('evidence/binary-check.json').read_text())

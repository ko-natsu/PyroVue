import gzip

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
input_file = ROOT / 'web' / 'src' / 'index.html'
output_file = ROOT / 'include' / 'ui_index.h'

html_content = input_file.read_bytes()
gzipped_content = gzip.compress(html_content, mtime=0)
hex_array = ', '.join([f'0x{byte:02x}' for byte in gzipped_content])
header_content = f'''#ifndef UI_INDEX_H
#define UI_INDEX_H

#include <stdint.h>
#include <pgmspace.h>

const uint8_t UI_INDEX_GZ[] PROGMEM = {{
    {hex_array}
}};
const size_t UI_INDEX_GZ_LEN = sizeof(UI_INDEX_GZ);

#endif // UI_INDEX_H
'''

output_file.write_text(header_content)

print(f'Successfully converted {input_file} to {output_file}')
print(f'Original size: {len(html_content)} bytes')
print(f'Gzipped size: {len(gzipped_content)} bytes')

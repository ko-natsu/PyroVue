import gzip

# Input and output file paths
input_file = '../UI-integration/kiln_live_ui_demo_single_file_mobile_friendly.html'
output_file = '../include/ui_index.h'

# Read the HTML file
with open(input_file, 'rb') as f_in:
    html_content = f_in.read()

# Gzip the content
gzipped_content = gzip.compress(html_content)

# Prepare the C++ header content
hex_array = ', '.join([f'0x{byte:02x}' for byte in gzipped_content])
header_content = f'''
#ifndef UI_INDEX_H
#define UI_INDEX_H

#include <pgmspace.h>

const uint8_t UI_INDEX_GZ[] PROGMEM = {{
    {hex_array}
}};
const size_t UI_INDEX_GZ_LEN = sizeof(UI_INDEX_GZ);

#endif // UI_INDEX_H
'''

# Write the header file
with open(output_file, 'w') as f_out:
    f_out.write(header_content)

print(f'Successfully converted {input_file} to {output_file}')
print(f'Original size: {len(html_content)} bytes')
print(f'Gzipped size: {len(gzipped_content)} bytes')

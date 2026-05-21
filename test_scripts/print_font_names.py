import struct

def show(path):
    with open(path, 'rb') as f:
        data = f.read()
    num_tables = struct.unpack('>H', data[4:6])[0]
    name_offset = None
    name_length = None
    for i in range(num_tables):
        rec = data[12 + i*16 : 12 + (i+1)*16]
        tag = rec[0:4].decode('latin-1')
        if tag == 'name':
            name_offset = struct.unpack('>I', rec[8:12])[0]
            name_length = struct.unpack('>I', rec[12:16])[0]
            break
    t = data[name_offset:name_offset+name_length]
    count = struct.unpack('>H', t[2:4])[0]
    str_offset = struct.unpack('>H', t[4:6])[0]
    out = {}
    for i in range(count):
        rec = t[6 + i*12 : 6 + (i+1)*12]
        platformID, encodingID, languageID, nameID, length, offset = struct.unpack('>HHHHHH', rec)
        if platformID == 3 and encodingID == 1:
            s = t[str_offset+offset : str_offset+offset+length].decode('utf-16-be', errors='ignore')
            out[nameID] = s
    print('Family (1):', repr(out.get(1)))
    print('Subfamily (2):', repr(out.get(2)))
    print('Full (4):', repr(out.get(4)))
    print('PostScript (6):', repr(out.get(6)))
    print('Typographic Family (16):', repr(out.get(16)))

print('--- Variable ---')
show('Font/Exo2-VariableFont_wght.ttf')

import struct, sys

data = open(r'c:\Users\xAstroBoy\Desktop\UE4-5 Quest Modloader\Pinball FX VR Patches\libUnreal_new.so', 'rb').read()

strings_to_find = [
    b'MaxObjectsNotConsideredByGC',
    b'OpenForDisregardForGC',
    b'SizeOfPermanentObjectPool',
    b'NumElementsPerChunk',
    b'FNamePool',
    b'PreAllocatedObjects',
]
for s in strings_to_find:
    idx = data.find(s)
    found = ('0x%X' % idx) if idx != -1 else 'NOT FOUND'
    print('%s: %s' % (s.decode(), found))

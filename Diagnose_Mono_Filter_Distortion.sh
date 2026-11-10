#!/bin/bash
# Read-only checks. Nothing is sent anywhere or modified.
plugin="$HOME/Library/Audio/Plug-Ins/CLAP/Mono Filter Distortion.clap"
binary="$plugin/Contents/MacOS/MonoFilterDistortion"

echo '===== macOS and architecture ====='
sw_vers
uname -m
echo
echo '===== Bundle and executable ====='
ls -ld "$plugin" "$plugin/Contents" "$plugin/Contents/MacOS" "$binary"
file "$binary"
echo
echo '===== Extended attributes ====='
xattr -lr "$plugin"
echo
echo '===== Signature verification ====='
codesign --verify --strict --verbose=4 "$plugin" 2>&1
echo
echo '===== Signature details ====='
codesign --display --verbose=4 "$plugin" 2>&1
echo
echo '===== Linked libraries ====='
otool -L "$binary" 2>&1
echo
echo '===== Standalone macOS bundle load ====='
if command -v python3 >/dev/null 2>&1; then
  python3 - "$plugin" "$binary" <<'PY'
import ctypes as C, os, sys

bundle_path, binary_path = sys.argv[1:]
try:
    C.CDLL(binary_path, mode=os.RTLD_NOW | os.RTLD_LOCAL)
    print('dlopen: success')
except OSError as e:
    print('dlopen:', e)

try:
    cf = C.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
    void = C.c_void_p
    cf.CFStringCreateWithCString.argtypes = [void,C.c_char_p,C.c_uint32]
    cf.CFStringCreateWithCString.restype = void
    cf.CFURLCreateWithFileSystemPath.argtypes = [void,void,C.c_int,C.c_bool]
    cf.CFURLCreateWithFileSystemPath.restype = void
    cf.CFBundleCreate.argtypes = [void,void]
    cf.CFBundleCreate.restype = void
    cf.CFBundleLoadExecutableAndReturnError.argtypes = [void,C.POINTER(void)]
    cf.CFBundleLoadExecutableAndReturnError.restype = C.c_bool
    cf.CFCopyDescription.argtypes = [void]
    cf.CFCopyDescription.restype = void
    cf.CFStringGetCString.argtypes = [void,C.c_char_p,C.c_long,C.c_uint32]
    cf.CFStringGetCString.restype = C.c_bool
    path = cf.CFStringCreateWithCString(None, os.fsencode(bundle_path), 0x08000100)
    url = cf.CFURLCreateWithFileSystemPath(None, path, 0, True)
    bundle = cf.CFBundleCreate(None, url)
    if not bundle:
        print('CFBundleCreate: failed')
    else:
        error = void()
        if cf.CFBundleLoadExecutableAndReturnError(bundle, C.byref(error)):
            print('CFBundleLoadExecutableAndReturnError: success')
        else:
            print('CFBundleLoadExecutableAndReturnError: failed')
            if error.value:
                description = cf.CFCopyDescription(error)
                text = C.create_string_buffer(32768)
                if description and cf.CFStringGetCString(description,text,len(text),0x08000100):
                    print(text.value.decode('utf-8','replace'))
except Exception as e:
    print('Could not run CoreFoundation diagnostic:',repr(e))
PY
else
  echo 'python3 is unavailable; skipped the two loader checks.'
fi

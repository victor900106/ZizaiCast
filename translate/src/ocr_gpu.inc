// OCR GPU add-on (DirectML): the DirectML build of onnxruntime and
// DirectML.dll, taken from Microsoft's NuGet packages without downloading
// the whole packages (Microsoft.AI.DirectML is 202 MB): each file's zip entry
// (local header + deflated data) is fetched with an HTTP Range request and
// unpacked with Windows' tar.exe.  Pinned: the byte range's SHA-256 and the
// unpacked file's size + SHA-256.  Licences: onnxruntime MIT (Microsoft);
// DirectML redistributable under the Microsoft DirectML licence terms (the
// user downloads it from Microsoft's package feed).
// PM_OCR_GPU_FILE(file, nuget path, entry name, offset, length, range sha256,
//                 crc32, compressed size, size, sha256, zip time, zip date)
PM_OCR_GPU_FILE("DirectML.dll", "/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg",
                "bin/x64-win/DirectML.dll", 142873380ull, 9332795ull,
                "881b74f738250da8b6ef896d745bdb3bc1371ea44dbfea78eeafbf56e6662449", 0x92489bc5u, 9332741u, 18527776ull,
                "9c9e6d822561c6c41b90e6994b3e8857cf1d66dbfb1e0c4c799c7c89b4e92da1", 0x1b13, 0x595a)
PM_OCR_GPU_FILE("onnxruntime.dll",
                "/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg",
                "runtimes/win-x64/native/onnxruntime.dll", 6342473ull, 6039180ull,
                "44e6b072d558dcd4d9abb5174a56f403f769b993493b1c88210e4dec34d562da", 0xc44e532du, 6039111u, 17328152ull,
                "e7eedec6a6f26dc39dc948276a75ef6d2bee3fff944d874ceed0bbd3b97bff40", 0x041b, 0x5c71)

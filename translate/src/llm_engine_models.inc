// Pinned downloads of the local LLM engine (llm_engine_store.cpp).  Size + SHA-256
// of every file are checked after download; a changed file is never used.
//
// Runtime: the official llama.cpp CPU build (MIT), GitHub release b11514 of
// ggml-org/llama.cpp (commit de7fa0a3c6a2e1b4cd9f22eb8d6bf5b12dbdb63b),
// asset llama-b11514-bin-win-cpu-x64.zip - the SHA-256 is the one GitHub
// lists for the asset.  Only the files below are unpacked (no .exe):
// llama.dll, ggml*.dll (ggml picks the ggml-cpu-<arch>.dll variant that fits
// the CPU at run time; only that one and ggml-cpu-x64 are downloaded), libomp.dll (LLVM OpenMP, Apache-2.0 WITH
// LLVM-exception) and its license.  The headers in llm_api/ are the same tag.
// PM_LLM_RUNTIME(archive name, size, sha256, host, path)
PM_LLM_RUNTIME("llama-b11514-bin-win-cpu-x64.zip", 19484487, "5a1d6ba1b414ab0234b12e29d911b3b8fe63051b4455215e184cf74870eac596", "github.com", "/ggml-org/llama.cpp/releases/download/b11514/llama-b11514-bin-win-cpu-x64.zip")
// PM_LLM_RUNTIME_FILE(name, size, sha256): checked after unpacking.
PM_LLM_RUNTIME_FILE("llama.dll", 3380224, "addb1956f93d78ed2e553e2f91e2c55085c2f001025af8240265ff1ed7b1c710")
PM_LLM_RUNTIME_FILE("ggml.dll", 79872, "7d5f3b3ff42254a30fdad4c2aff46986d9079c5ea21ea9c82b59e21cfe3c7c8f")
PM_LLM_RUNTIME_FILE("ggml-base.dll", 801792, "0b4bba29a48b4dc1b51b303edc2d610c97367de2e65fff2a089ffee99037fde8")
PM_LLM_RUNTIME_FILE("ggml-cpu-alderlake.dll", 1435136, "25494a38cabf44cea66b82b4a08769baabb58c754b0a0ad8c1a460a644e2e55f")
PM_LLM_RUNTIME_FILE("ggml-cpu-cannonlake.dll", 1614848, "a78ac16d0888987ed7542a21c89bded60be8521c594abfdb8f9e546dadd43a2f")
PM_LLM_RUNTIME_FILE("ggml-cpu-cascadelake.dll", 1599488, "276e3573584ef6d1fb7eee09ffc564f83136f44edf678d56fd81c7d621aff943")
PM_LLM_RUNTIME_FILE("ggml-cpu-cooperlake.dll", 1609216, "ea301f24cced6ba4ea8a676e07cf62fa0ee455bca549df00281b92fa1c6fa1a0")
PM_LLM_RUNTIME_FILE("ggml-cpu-haswell.dll", 1439744, "4c6187afc65b1ba728f6af5c77a5f6be14f76a9b3f3b11707095de5a5033ce50")
PM_LLM_RUNTIME_FILE("ggml-cpu-icelake.dll", 1606656, "6f2bff8f103e0806d4ce9f6e0172f87481a8be1995fbdb19629c1967dfa8bdbd")
PM_LLM_RUNTIME_FILE("ggml-cpu-ivybridge.dll", 1315328, "039e9df8cea7e802740055d80d6cd82547fc587d34ebd913d4018371eff0e17d")
PM_LLM_RUNTIME_FILE("ggml-cpu-piledriver.dll", 1308160, "039cb26ddcbe7c547d7880121d51898142a208f2ccff0bc79177cb925e3b7b0a")
PM_LLM_RUNTIME_FILE("ggml-cpu-sandybridge.dll", 1290240, "43fe023afa769749dc319df2ca81ed84aa7c35668e434da628e9872246936e66")
PM_LLM_RUNTIME_FILE("ggml-cpu-sapphirerapids.dll", 1886208, "78b71c9c64ba5b1240fdda0a5743683f9590876a475c489af601e69aaadf0bdb")
PM_LLM_RUNTIME_FILE("ggml-cpu-skylakex.dll", 1608704, "7c936a4f20b51191eb5db95adc1e4d266d53d2608d1f715751dc4e3b415f732d")
PM_LLM_RUNTIME_FILE("ggml-cpu-sse42.dll", 915968, "bd042c2089b5d8877e843c71ea78a846a94e82c4e8c8dce0cdb5e5cee78d0209")
PM_LLM_RUNTIME_FILE("ggml-cpu-x64.dll", 908800, "861e77580f1a9e46b1da3135715bb4fdaf58bab7784fb781776014f25c8dde1b")
PM_LLM_RUNTIME_FILE("ggml-cpu-zen4.dll", 1615872, "e06fc2dc12da9c063d0feff6d05c23b399268bfb5e19335d1b6fe1bd29d2b896")
PM_LLM_RUNTIME_FILE("libomp.dll", 768000, "a12116ba72d1d6820407cf30be23da04ce79d6bb8a71a5ee71759c5a1faa6f1c")
PM_LLM_RUNTIME_FILE("LICENSE-LLVM-OpenMP", 19741, "fdad1758a9e1f9d5a81e18879b3406772115edc92c24bfa36b70c654f325e8e4")

// PM_LLM_RUNTIME_ENTRY(name, offset, length, sha256): where the file's zip
// record (local header + deflated data) lies in the archive above, and the
// SHA-256 of exactly those bytes.  The runtime is fetched as these byte ranges
// (HTTP Range; only the files this PC needs), the whole archive only when the
// server cannot do Range.  Generated: py tools/make_llm_runtime_entries.py ZIP
PM_LLM_RUNTIME_ENTRY("llama.dll", 17676892ull, 1211470ull, "08b9b9be9df222b0f69c66f15f4217cc31c7d3e5ed57bef35537ee0d68aea840")
PM_LLM_RUNTIME_ENTRY("ggml.dll", 7630727ull, 30978ull, "e0cd5e4bdbf86d7f66072ff4a4736e6538ac505cd418258b1bfe4528d3ffd684")
PM_LLM_RUNTIME_ENTRY("ggml-base.dll", 0ull, 308577ull, "008149f2be46487c0442c423b7e044089883050433671487fe2e11ae2be84fa6")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-alderlake.dll", 308577ull, 525068ull, "1e0b5d0d9f7b4b64cb52758e3733263621dcd892c028d147f418a00384b9c21f")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-cannonlake.dll", 833645ull, 573847ull, "9e35ecfbff6ac70a4528329d973e584d0defd94b5f2e1723b43e5b7a6f653ed7")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-cascadelake.dll", 1407492ull, 571359ull, "967b10b7e3d7d1963b4ddddcf0512662f578589faa126633bed1c8d08a8accb9")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-cooperlake.dll", 1978851ull, 574713ull, "b40092455ccafabfb43f204f0e77d1264246dae4fdfb8e022c2f0a5faf2c055a")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-haswell.dll", 2553564ull, 526760ull, "001c41d6b9d716b0aaf8e636b8d0581a73ad1e9b3638fbfcb5e586a34eeee820")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-icelake.dll", 3080324ull, 574888ull, "875eae01f53c16698b40fec93408d01a6eab8ac57c16ab2eb5a841ad5228477c")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-ivybridge.dll", 3655212ull, 459769ull, "01b6f86ed906c6f854f655a94f3a01291a21699c8ea178155f952a3be42b1fda")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-piledriver.dll", 4114981ull, 459444ull, "388c62aedd06dd42c41a522612b6c62ad31d5474b1ebd18fa890a6ce92d9a309")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-sandybridge.dll", 4574425ull, 451582ull, "8ad6a8e18607f30d5ac941dff1ff744914151349a46200a51b359c8c0a8e4b2e")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-sapphirerapids.dll", 5026007ull, 660526ull, "87b01d65be0a1ed5e223ae3ee819732485abb26b2aaab0fb9e3a16814c6745ae")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-skylakex.dll", 5686533ull, 570452ull, "3951538a60ba72210ac9c8b3e1aee7aa75e3b788e767668918acb9b571137b52")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-sse42.dll", 6256985ull, 336788ull, "c2dc3745983c4fec60e3e35d1392d73c7ee8a534cd6dc5fdba59e979e2fe373a")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-x64.dll", 6593773ull, 332805ull, "3a4fae1bf9352c83b692577e85e9636d30533f47f8bd8bd8da567f849071bbd0")
PM_LLM_RUNTIME_ENTRY("ggml-cpu-zen4.dll", 6926578ull, 578420ull, "c67905d5752985616564fc1a93543225da6a8770349cd3e412cf15196430f6e4")
PM_LLM_RUNTIME_ENTRY("libomp.dll", 7661705ull, 343811ull, "a8229d5f64f62cabe042253fbbdbe71198686e834e319f13748a1529aa658fe1")
PM_LLM_RUNTIME_ENTRY("LICENSE-LLVM-OpenMP", 8005516ull, 5853ull, "537257da711084cd0b5f35622cd6815d0ec9f11737eb67308d0a0271ab6319ea")

// Optional GPU backend: ggml-vulkan.dll from the Vulkan build of the same
// release (its other DLLs are byte-identical to the CPU build's), unpacked
// next to them; ggml loads it at start when a Vulkan driver is present.
// PM_LLM_GPU(archive name, size, sha256, host, path)
PM_LLM_GPU("llama-b11514-bin-win-vulkan-x64.zip", 33446009, "e1eaddcde761b1f839b9c6d5924b97f2698fba22068ca3b63f129c3caf55b88f", "github.com", "/ggml-org/llama.cpp/releases/download/b11514/llama-b11514-bin-win-vulkan-x64.zip")
PM_LLM_GPU_FILE("ggml-vulkan.dll", 45362688, "ac958a9b4c7152c45b52424d3901fb438c160dca08aa312a9cfcde3fbefd5f6c")

// Models: GGUF files at a fixed commit of their Hugging Face repo (not gated,
// no login).  The first one is the default.  ramMB: peak working set while
// translating (pm_llm_eval, 2048-token context).
// PM_LLM_MODEL(id, family, file, size, sha256, host, path, license, name, ramMB)
PM_LLM_MODEL("qwen3.5-2b-q4km", "chatml", "Qwen3.5-2B-Q4_K_M.gguf", 1280835840, "aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223", "huggingface.co", "/unsloth/Qwen3.5-2B-GGUF/resolve/f6d5376be1edb4d416d56da11e5397a961aca8ae/Qwen3.5-2B-Q4_K_M.gguf", "Apache-2.0", "Qwen3.5-2B (Qwen, Q4_K_M)", 1700)
PM_LLM_MODEL("qwen3-1.7b-q4km", "chatml", "Qwen3-1.7B-Q4_K_M.gguf", 1282439264, "d2387ca2dbfee2ffabce7120d3770dadca0b293052bc2f0e138fdc940d9bc7b5", "huggingface.co", "/ggml-org/Qwen3-1.7B-GGUF/resolve/daeb8e2d528a760970442092f6bf1e55c3b659eb/Qwen3-1.7B-Q4_K_M.gguf", "Apache-2.0", "Qwen3-1.7B (Qwen, Q4_K_M)", 1700)
PM_LLM_MODEL("qwen3.5-0.8b-q4", "chatml", "Qwen3.5-0.8B-Q4_0.gguf", 563036064, "57d1997790d1744fba5b40a7317df71ea5e2acee28c47e78f0cce39c0703f8cf", "huggingface.co", "/ggml-org/Qwen3.5-0.8B-GGUF/resolve/8fea620810c4afa23dd6443f999a48574c1611a3/Qwen3.5-0.8B-Q4_0.gguf", "Apache-2.0", "Qwen3.5-0.8B (Qwen, Q4_0)", 900)

// Pinned downloads of the local LLM engine (llm_engine_store.cpp).  Size + SHA-256
// of every file are checked after download; a changed file is never used.
//
// Runtime: the official llama.cpp CPU build (MIT), GitHub release b11514 of
// ggml-org/llama.cpp (commit de7fa0a3c6a2e1b4cd9f22eb8d6bf5b12dbdb63b),
// asset llama-b11514-bin-win-cpu-x64.zip - the SHA-256 is the one GitHub
// lists for the asset.  Only the files below are unpacked (no .exe):
// llama.dll, ggml*.dll (ggml picks the ggml-cpu-<arch>.dll variant that fits
// the CPU at run time), libomp.dll (LLVM OpenMP, Apache-2.0 WITH
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

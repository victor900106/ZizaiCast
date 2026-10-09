llama.cpp / ggml public headers, unmodified, from tag b11514
(https://github.com/ggml-org/llama.cpp/tree/b11514, commit
de7fa0a3c6a2e1b4cd9f22eb8d6bf5b12dbdb63b), MIT license (LICENSE).

They must match the downloaded runtime (llm_engine_models.inc: the b11514
"bin-win-cpu-x64" release): llm_engine.cpp loads llama.dll at run time and
passes these structs by value.  Update both together
(copy include/*.h + ggml/include/*.h of the new tag, then update the pinned sizes / SHA-256 in llm_engine_models.inc).

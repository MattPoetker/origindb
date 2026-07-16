// Compiles the unit-test WAT fixture to a .wasm file for e2e testing.
#include <wasmtime.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>
int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s in.wat out.wasm\n", argv[0]); return 1; }
    std::ifstream in(argv[1]);
    std::stringstream ss; ss << in.rdbuf();
    std::string wat = ss.str();
    wasm_byte_vec_t out;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat.c_str(), wat.size(), &out);
    if (err) { fprintf(stderr, "wat2wasm failed\n"); return 1; }
    std::ofstream of(argv[2], std::ios::binary);
    of.write(out.data, out.size);
    wasm_byte_vec_delete(&out);
    printf("wrote %s\n", argv[2]);
    return 0;
}

// fury: the wasm JIT spike. Proves the mechanism a dynrec wasm backend needs
// inside the js-dos worker build: build a wasm module's bytes in C++ at run
// time, instantiate it synchronously with the main module's memory and
// table imported, put its function in the table, and call it from C++
// through a function pointer -- including a call_indirect from the
// generated code back into a C helper.
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <emscripten.h>

EM_JS(int, fury_jit_instantiate, (const uint8_t* bytes, int len), {
  try {
    const mod = new WebAssembly.Module(HEAPU8.subarray(bytes, bytes + len));
    const inst = new WebAssembly.Instance(mod, { env: { memory: wasmMemory, table: wasmTable } });
    const idx = wasmTable.grow(1);
    wasmTable.set(idx, inst.exports.f);
    return idx;
  } catch (e) {
    err("fury_jit_instantiate: " + e);
    return 0;
  }
});

namespace {
struct Buf {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void uleb(uint32_t v) { do { uint8_t c = v & 0x7f; v >>= 7; if (v) c |= 0x80; b.push_back(c); } while (v); }
  void str(const char* s) { uint32_t n = 0; while (s[n]) n++; uleb(n); for (uint32_t i = 0; i < n; i++) u8((uint8_t)s[i]); }
  void section(uint8_t id, const Buf& body) { u8(id); uleb((uint32_t)body.b.size()); b.insert(b.end(), body.b.begin(), body.b.end()); }
};
}

extern "C" int fury_jit_helper(int x) { return x * 10; }

// f(a, b, helper) = helper(a + b), helper called through the shared table
static std::vector<uint8_t> spikeModule() {
  Buf m;
  m.u8(0); m.u8('a'); m.u8('s'); m.u8('m'); m.u8(1); m.u8(0); m.u8(0); m.u8(0);
  Buf t;                         // type section: 0 = (i32,i32,i32)->i32, 1 = (i32)->i32
  t.uleb(2);
  t.u8(0x60); t.uleb(3); t.u8(0x7f); t.u8(0x7f); t.u8(0x7f); t.uleb(1); t.u8(0x7f);
  t.u8(0x60); t.uleb(1); t.u8(0x7f); t.uleb(1); t.u8(0x7f);
  m.section(1, t);
  Buf im;                        // imports: env.memory, env.table
  im.uleb(2);
  im.str("env"); im.str("memory"); im.u8(0x02); im.u8(0x00); im.uleb(0);
  im.str("env"); im.str("table");  im.u8(0x01); im.u8(0x70); im.u8(0x00); im.uleb(0);
  m.section(2, im);
  Buf f; f.uleb(1); f.uleb(0); m.section(3, f);          // one function of type 0
  Buf e; e.uleb(1); e.str("f"); e.u8(0x00); e.uleb(0); m.section(7, e);
  Buf body;                      // locals: none; a+b; helper; call_indirect type 1 table 0
  body.uleb(0);
  body.u8(0x20); body.uleb(0);
  body.u8(0x20); body.uleb(1);
  body.u8(0x6a);
  body.u8(0x20); body.uleb(2);
  body.u8(0x11); body.uleb(1); body.uleb(0);
  body.u8(0x0b);
  Buf c; c.uleb(1); c.uleb((uint32_t)body.b.size()); c.b.insert(c.b.end(), body.b.begin(), body.b.end());
  m.section(10, c);
  return m.b;
}

extern "C" void fury_jit_selftest();
void fury_jit_selftest() {
  std::vector<uint8_t> bytes = spikeModule();
  int idx = fury_jit_instantiate(bytes.data(), (int)bytes.size());
  if (!idx) { printf("FURY_JIT selftest: instantiate failed (%d bytes)\n", (int)bytes.size()); return; }
  typedef int (*fn_t)(int, int, int);
  fn_t fn = (fn_t)(uintptr_t)idx;
  int helper = (int)(uintptr_t)&fury_jit_helper;
  int r = fn(3, 4, helper);
  printf("FURY_JIT selftest: module %d bytes, table index %d, helper index %d, f(3,4)=%d %s\n",
         (int)bytes.size(), idx, helper, r, r == 70 ? "OK" : "WRONG");
  // a file in the mounted root: the check page reads it back
  FILE* f = fopen("jit.txt", "w");
  if (f) {
    fprintf(f, "FURY_JIT selftest: module %d bytes, table index %d, helper index %d, f(3,4)=%d %s\n",
            (int)bytes.size(), idx, helper, r, r == 70 ? "OK" : "WRONG");
    fclose(f);
  }
}

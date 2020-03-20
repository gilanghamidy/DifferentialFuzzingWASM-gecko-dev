#include "jsapi-ext.h"

#include "vm/NativeObject.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmTypes.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmTypes.h"
#include "wasm/WasmInstance.h"
#include <iostream>
#include <algorithm>

class js::ext::CompiledInstructions::Internal {
  friend std::unique_ptr<js::ext::CompiledInstructions>
              js::ext::CompileWasmBytes(JSContext* cx, uint8_t const* arr, size_t size);
  friend class js::ext::CompiledInstructions;

  js::RootedWasmInstanceObject instance;

  Internal(JSContext* cx) : instance(cx) { }
};

bool js::ext::CompiledInstructions::Function::Invoke(JSContext* cs, std::vector<JS::Value>& argsStack) {
  auto& moduleInstance = parent->internal->instance;
  js::wasm::Instance& instance = moduleInstance->instance();
  std::cout << "Invoke: " << this->index << "argStack.size: " << argsStack.size() << std::endl;
  return instance.callExport(cs, this->index, JS::CallArgsFromVp(argsStack.size(), argsStack.data()));
}

js::ext::CompiledInstructions::CompiledInstructions(JSContext* cx) {
  this->internal = new Internal(cx);
}

JS_PUBLIC_API js::ext::CompiledInstructions::~CompiledInstructions() {
  delete this->internal;
}

JS_PUBLIC_API std::unique_ptr<js::ext::CompiledInstructions>
              js::ext::CompileWasmBytes(JSContext* cx, uint8_t const* arr, size_t size) {

  using namespace js::wasm;

  if (!GlobalObject::ensureConstructor(cx, cx->global(), JSProto_WebAssembly)) {
    return { nullptr };
  }

  MutableBytes bytecode = cx->new_<ShareableBytes>();
  if (!bytecode) {
    return { nullptr };
  }

  if (!bytecode->append(arr, size)) {
    ReportOutOfMemory(cx);
    return { nullptr };
  }

  ScriptedCaller scriptedCaller;

  SharedCompileArgs compileArgs =
      CompileArgs::build(cx, std::move(scriptedCaller));
  if (!compileArgs) {
    return { nullptr };
  }

  UniqueChars error;
  UniqueCharsVector warnings;
  SharedModule module =
      CompileBuffer(*compileArgs, *bytecode, &error, &warnings);
  if (!module) {
    if (error) {
      std::cerr << "ERROR: " << error.get() << std::endl;
      return { nullptr };
    }
    ReportOutOfMemory(cx);
    return { nullptr };
  }

  auto retObj = std::make_unique<js::ext::CompiledInstructions>(cx);

  auto& wasmCode = module->code();
  auto& codeTierMeta = wasmCode.codeTier(js::wasm::Tier::Optimized).metadata();
  auto& codesegment = module->code().codeTier(js::wasm::Tier::Optimized).segment();
  auto& moduleExports = module->exports();
  auto baseaddress = codesegment.base();
  auto len = codesegment.length();

  Rooted<ImportValues> imports(cx);

  

  // Instantiate the module
  if(!module->instantiate(cx, imports.get(), nullptr, &retObj->internal->instance))
    return { };

  for(auto& codeRange : codeTierMeta.codeRanges) {
    if(codeRange.isFunction()) {
      auto& dumpedFunction = retObj->functions_.emplace_back();
      auto codeBegin = baseaddress + codeRange.funcNormalEntry();
      auto codeEnd = baseaddress + codeRange.end();
      dumpedFunction.parent = retObj.get();
      dumpedFunction.instructions.insert(dumpedFunction.instructions.end(), codeBegin, codeEnd);
      dumpedFunction.index = codeRange.funcIndex();

      auto funcExport = std::find_if(moduleExports.begin(),
                                     moduleExports.end(),
                                     [idx = codeRange.funcIndex()] (Export const& a) { return a.funcIndex() == idx; });

      if(funcExport != moduleExports.end()) {
        dumpedFunction.exported = true;
        dumpedFunction.name = funcExport->fieldName();
        retObj->functionsByName.emplace(dumpedFunction.name, std::ref(dumpedFunction));
      }
    }
  }


  /*
  std::cout << "EXPORTED FUNC: \n";
  for(js::wasm::Export const& ex : module->exports()) {
    std::cout << "Func: " << ex.fieldName() << ", idx: " << ex.funcIndex() << ", entry: " << *module->code().getAddressOfJitEntry(ex.funcIndex()) << std::endl;

    auto& funcExport = codeTierMeta.lookupFuncExport(ex.funcIndex());
    auto& codeRange = codeTierMeta.codeRange(funcExport);

    auto ptr = codeRange.begin();
    auto end = codeRange.end();
    std::cout << "BEGIN FUNC " << std::endl;
    while(ptr != end) {
      printf("%02X ", *(baseaddress + ptr));
      ptr++;
    }
    std::cout << " END" << std::endl;
  }



  std::cout << "DUMPDED FUNC: \n";
  for(uint32_t i = 0; i < len; i++) {
    printf("%02X ", baseaddress[i]);
  }
  std::cout << " END" << std::endl;
  */

  return retObj;
}

JS::Value js::ext::CreateBigIntValue(JSContext* cs, uint64_t val) {
  auto bigint = JS::BigInt::createFromUint64(cs, val);
  return JS::BigIntValue(bigint);
}

uint64_t js::ext::GetBigIntValue(JS::Value val) {
  return JS::BigInt::toUint64(val.toBigInt());
}
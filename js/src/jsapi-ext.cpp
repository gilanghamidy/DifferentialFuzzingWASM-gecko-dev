#include "jsapi-ext.h"

#include <algorithm>
#include <iostream>

#include "vm/NativeObject.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmTypes.h"



class js::ext::CompiledInstructions::Internal {
  friend std::unique_ptr<js::ext::CompiledInstructions>
              js::ext::CompileWasmBytes(JSContext* cx, uint8_t const* arr, size_t size);
  friend class js::ext::CompiledInstructions;

  js::RootedWasmInstanceObject instance;
  js::wasm::SharedModule module;
  js::WasmMemoryObject* wasm_memory { nullptr };
  
public:
  Internal(JSContext* cx) : instance(cx) { }
};

bool js::ext::CompiledInstructions::Function::Invoke(JSContext* cs, std::vector<JS::Value>& argsStack) {  
  if(parent->internal->instance.get() == nullptr)
    return false;

  auto& moduleInstance = parent->internal->instance;
  js::wasm::Instance& instance = moduleInstance->instance();
  return instance.callExport(cs, this->index, JS::CallArgsFromVp(argsStack.size(), argsStack.data()));
}

js::ext::CompiledInstructions::CompiledInstructions(JSContext* cx) {
  this->internal = std::make_unique<Internal>(cx);
}

JS_PUBLIC_API js::ext::CompiledInstructions::~CompiledInstructions() {
}

js::ext::WasmType ValTypeToExtType(js::wasm::ValType::Kind valTypeKind) {
  using Kind = js::wasm::ValType::Kind;
  using WasmType = js::ext::WasmType;
  switch(valTypeKind) {
    case Kind::I32: return WasmType::I32;
    case Kind::I64: return WasmType::I64;
    case Kind::F32: return WasmType::F32;
    case Kind::F64: return WasmType::F64;
  }
  MOZ_ASSERT_UNREACHABLE("Invalid ValType::Kind");
}

bool js::ext::CompiledInstructions::InstantiateWasm(JSContext* cx) {
  // Instantiate the module
  Rooted<js::wasm::ImportValues> imports(cx);
  
  // Add memory
  if(this->internal->wasm_memory != nullptr)
    imports.get().memory = this->internal->wasm_memory;

  if(!this->internal->module->instantiate(
            cx, imports.get(), nullptr, &this->internal->instance))
    return false;

  return true;
}

void JS_PUBLIC_API js::ext::CompiledInstructions::NewMemoryImport(JSContext* cx) {
  mozilla::Maybe<uint32_t> maxSize;
  mozilla::Maybe<size_t> mappedSize;
  const wasm::Metadata& metadata = this->internal->module->metadata();
  const wasm::MetadataCacheablePod& pod = metadata.pod();
  
  /*
  auto wasm_buffer = 
        js::WasmArrayRawBuffer::Allocate(pod.minMemoryLength, 
                                         pod.maxMemoryLength, 
                                         mappedSize);
        
  js::Rooted<js::ArrayBufferObject*> array_buffer { cx };
  array_buffer.set(js::ArrayBufferObject::createFromNewRawBuffer(
                                        cx, wasm_buffer, 
                                        pod.minMemoryLength));

  JS::RootedObject proto(
        cx, &cx->global()->getPrototype(JSProto_WasmMemory).toObject());

  this->internal->wasm_memory = js::WasmMemoryObject::create(
                                        cx, {array_buffer}, proto);

  MOZ_ASSERT(!metadata().isAsmJS());
  */

  uint32_t declaredMin = metadata.minMemoryLength;
  mozilla::Maybe<uint32_t> declaredMax = metadata.maxMemoryLength;

  RootedArrayBufferObjectMaybeShared buffer(cx);
  wasm::Limits l(declaredMin, declaredMax, wasm::Shareable::False);
  if (!CreateWasmBuffer(cx, l, &buffer)) {
    std::cout << "Error CreateWasmBuffer\n";
    return;
  }

  RootedObject proto(
      cx, &cx->global()->getPrototype(JSProto_WasmMemory).toObject());
  this->internal->wasm_memory = WasmMemoryObject::create(cx, buffer, proto);
}

auto JS_PUBLIC_API js::ext::CompiledInstructions::GetWasmMemory() -> WasmMemoryRef {
  if(this->internal->wasm_memory == nullptr ) {
    return { nullptr };
  }
  auto data = this->internal->wasm_memory->buffer().dataPointerEither();
  return { data.unwrap(), this->internal->wasm_memory->volatileMemoryLength() };
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

  retObj->internal->module = module;

  auto& wasmCode = module->code();
  auto& codeTierMeta = wasmCode.codeTier(js::wasm::Tier::Optimized).metadata();
  auto& codesegment = module->code().codeTier(js::wasm::Tier::Optimized).segment();
  auto& moduleExports = module->exports();
  auto baseaddress = codesegment.base();
  auto len = codesegment.length();
  auto& funcExports = codeTierMeta.funcExports;

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

      auto funcExportMeta = std::find_if(funcExports.begin(),
                                         funcExports.end(),
                                         [idx = codeRange.funcIndex()] (FuncExport const& a) { return a.funcIndex() == idx; });
      
      if(funcExport != moduleExports.end()) {
        dumpedFunction.exported = true;
        dumpedFunction.name = funcExport->fieldName();
        retObj->functionsByName.emplace(dumpedFunction.name, std::ref(dumpedFunction));
      }

      if(funcExportMeta != funcExports.end()) {
        FuncType const& funcType = funcExportMeta->funcType();
        auto returnTypeMaybe = funcType.ret();

        // Get Return Type information
        // TODO: Change return type to vector instead of singleton
        dumpedFunction.returnType = returnTypeMaybe.isNothing()
                                    ? WasmType::Void
                                    : ValTypeToExtType(returnTypeMaybe->kind());
        // Get parameters
        for(decltype(auto) kind : funcType.args()) {
          dumpedFunction.parameters.push_back(ValTypeToExtType(kind.kind()));
        }
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
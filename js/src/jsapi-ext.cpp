#include "jsapi-ext.h"

#include <algorithm>
#include <chrono>
#include <iostream>


#include "vm/NativeObject.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmTypes.h"

#include "vm/ArrayBufferObject-inl.h"

struct GlobalEntry {
  js::ext::WasmType type;
  js::WasmGlobalObject* global_object;
};

class js::ext::CompiledInstructions::Internal {
  friend std::unique_ptr<js::ext::CompiledInstructions>
              js::ext::CompileWasmBytes(JSContext* cx, uint8_t const* arr, size_t size);
  friend class js::ext::CompiledInstructions;

  js::RootedWasmInstanceObject instance;
  js::wasm::SharedModule module;
  js::WasmMemoryObject* wasm_memory { nullptr };
  std::map<std::string, GlobalEntry> wasm_global_list;
  js::WasmGlobalObjectVector global_vector;
  bool global_import_processed { false };
public:
  Internal(JSContext* cx) : instance(cx) { }
};

std::tuple<bool, uint64_t>  js::ext::CompiledInstructions::Function::Invoke(JSContext* cs, std::vector<JS::Value>& argsStack) {  
  if(parent->internal->instance.get() == nullptr)
    return {false, 0};

  auto& moduleInstance = parent->internal->instance;
  js::wasm::Instance& instance = moduleInstance->instance();
  auto callargs = JS::CallArgsFromVp(argsStack.size(), argsStack.data());

  std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
  bool res = instance.callExport(cs, this->index, callargs);
  std::chrono::steady_clock::time_point end_time = std::chrono::steady_clock::now();
  
  uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
  
  return {res, elapsed};
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
  
  if(this->internal->global_vector.length() != 0) {
    imports.get().globalObjs.resize(this->internal->global_vector.length());
    for(int i = 0; i < this->internal->global_vector.length(); ++i) {
      imports.get().globalObjs[i] = this->internal->global_vector[i];
    }
  }

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

void JS_PUBLIC_API js::ext::CompiledInstructions::NewGlobalImport(JSContext* cx) {
  if(this->internal->global_import_processed)
    return;
  this->internal->global_import_processed = true;

  auto& moduleImports = this->internal->module->imports();
  const wasm::Metadata& metadata = this->internal->module->metadata();
  uint32_t globalIndex = 0;
  const wasm::GlobalDescVector& globals = metadata.globals;

  for(js::wasm::Import const& importEntry : moduleImports) {
    if(importEntry.kind == js::wasm::DefinitionKind::Global) {
      uint32_t this_index = globalIndex++;
      wasm::GlobalDesc const& this_desc = globals[this_index];
      MOZ_ASSERT(this_desc.isImport());

      wasm::ValType this_type = this_desc.type();

      // Create initial value based on type(source: WasmJS.cpp:550-557)
      wasm::RootedVal val(cx);
      val.set(wasm::Val(this_type));

      // Create WasmGlobalObject with the specified global type
      RootedObject proto(cx);
      proto = GlobalObject::getOrCreatePrototype(cx, JSProto_WasmGlobal);
      WasmGlobalObject* this_global = WasmGlobalObject::create(cx, val, this_desc.isMutable(), proto);

      // Store in WasmGlobalObjectVector based on its index
      if (this->internal->global_vector.length() <= this_index &&
          !this->internal->global_vector.resize(this_index + 1)) {
        ReportOutOfMemory(cx);
        return;
      }
      this->internal->global_vector[this_index] = this_global;
      this->internal->wasm_global_list.emplace(
          std::string{importEntry.field.get()}, 
          GlobalEntry {ValTypeToExtType(this_type.kind()), this_global });

      this->globals_.emplace(std::string{importEntry.field.get()}, ValTypeToExtType(this_type.kind()));

    }
  }
}

void js::ext::CompiledInstructions::SetGlobalImport(JSContext* cx, std::string const& name, WasmGlobalArg value) {
  auto& global_list = this->internal->wasm_global_list;
  auto global_iter = global_list.find(name);
  if(global_iter != global_list.end()) {
    using E = js::ext::WasmType;
    GlobalEntry& global_ = global_iter->second;
    wasm::RootedVal val(cx);
    
    switch(global_.type) {
      case E::I32: val.set(wasm::Val(value.i32)); break;
      case E::I64: val.set(wasm::Val(value.i64)); break;
      case E::F32: val.set(wasm::Val(value.f32)); break;
      case E::F64: val.set(wasm::Val(value.f64)); break;
      case E::Void: MOZ_CRASH();
    }
    global_.global_object->setVal(cx, val);
  }
}

auto js::ext::CompiledInstructions::GetGlobalImport(JSContext* cx, std::string const& name) 
  -> std::pair<WasmType, WasmGlobalArg> {
  using E = js::ext::WasmType;
  auto& global_list = this->internal->wasm_global_list;
  auto global_iter = global_list.find(name);
  if(global_iter != global_list.end()) {
    
    GlobalEntry& global_ = global_iter->second;
    wasm::RootedVal val(cx);
    global_.global_object->val(&val);

    WasmGlobalArg value;
    switch(global_.type) {
      case E::I32: value.i32 = val.get().i32(); break;
      case E::I64: value.i64 = val.get().i64(); break;
      case E::F32: value.f32 = val.get().f32(); break;
      case E::F64: value.f64 = val.get().f64(); break;
      case E::Void: MOZ_CRASH();
    }
    return {global_.type, value};
  } else {
    return {E::Void, {}};
  }
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

        // Get Return Type information
        // TODO: Change return type to vector instead of singleton
        decltype(auto) resultTypes = funcType.results();
        if(resultTypes.empty()) {
          dumpedFunction.returnType = WasmType::Void;
        } else {
          // Only take the first one
          dumpedFunction.returnType = ValTypeToExtType(resultTypes[0].kind()); 
        }

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
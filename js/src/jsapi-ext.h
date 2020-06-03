#ifndef jsapi_ext_h
#define jsapi_ext_h

/**
 * EXTENSION FOR INSTRUMENTATION PURPOSE
 */

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "jsapi.h"

namespace js {
namespace ext {

class CompiledInstructions;

extern JS_PUBLIC_API std::unique_ptr<CompiledInstructions> CompileWasmBytes(JSContext* cs, uint8_t const* arr, size_t size);

extern JS_PUBLIC_API JS::Value CreateBigIntValue(JSContext* cs, uint64_t val);

extern JS_PUBLIC_API uint64_t GetBigIntValue(JS::Value val);

enum class WasmType {
  Void,
  I32,
  I64,
  F32,
  F64
};

class CompiledInstructions {
public:
    struct Function {
        std::string name;
        std::vector<uint8_t> instructions;
        bool exported;
        uint32_t index;
        CompiledInstructions* parent;
        
        // Reflection info
        std::vector<WasmType> parameters;
        WasmType returnType;

        bool JS_PUBLIC_API Invoke(JSContext* cs, std::vector<JS::Value>& argsStack);
    };

private:
    friend std::unique_ptr<CompiledInstructions> js::ext::CompileWasmBytes(JSContext* cs, uint8_t const* arr, size_t size);
    class Internal;
    std::unique_ptr<Internal> internal;
    std::list<Function> functions_;
    std::map<std::string, WasmType> globals_;
    std::map<std::string, std::reference_wrapper<Function>> functionsByName;

public:
    struct WasmMemoryRef {
        uint8_t* buffer;
        size_t length;
    };

    union WasmGlobalArg {
        uint32_t i32;
        uint64_t i64;
        float   f32;
        double  f64;
    };

    CompiledInstructions(JSContext*);
    JS_PUBLIC_API ~CompiledInstructions();
    std::list<Function> const& functions() { return functions_; }
    std::map<std::string, WasmType> const& Globals() const noexcept { return globals_; }

    bool JS_PUBLIC_API InstantiateWasm(JSContext* cx);

    void JS_PUBLIC_API NewMemoryImport(JSContext* cx);
    void JS_PUBLIC_API NewGlobalImport(JSContext* cx);

    void JS_PUBLIC_API SetGlobalImport(JSContext* cx, std::string const& name, WasmGlobalArg value);
    std::pair<WasmType, WasmGlobalArg> JS_PUBLIC_API GetGlobalImport(JSContext* cx, std::string const& name);

    WasmMemoryRef JS_PUBLIC_API GetWasmMemory();

    Function* operator[](std::string const& arg) { 
        auto funcIter = functionsByName.find(arg);
        return funcIter != functionsByName.end() ? &funcIter->second.get() : nullptr; 
    }
};



}
}

#endif
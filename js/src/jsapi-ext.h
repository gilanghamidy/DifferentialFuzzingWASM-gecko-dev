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

class CompiledInstructions {
public:
    struct Function {
        std::string name;
        std::vector<uint8_t> instructions;
        bool exported;
        uint32_t index;
        CompiledInstructions* parent;
        bool JS_PUBLIC_API Invoke(JSContext* cs, std::vector<JS::Value>& argsStack);
    };

private:
    friend std::unique_ptr<CompiledInstructions> js::ext::CompileWasmBytes(JSContext* cs, uint8_t const* arr, size_t size);
    class Internal;
    Internal* internal;
    std::list<Function> functions_;
    std::map<std::string, std::reference_wrapper<Function>> functionsByName;

public:
    CompiledInstructions(JSContext*);
    ~CompiledInstructions();
    std::list<Function> const& functions() { return functions_; }
    Function& operator[](std::string const& arg) { return functionsByName.find(arg)->second.get(); }
};



}
}

#endif
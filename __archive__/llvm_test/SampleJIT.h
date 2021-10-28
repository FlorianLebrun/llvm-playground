
#ifndef LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H
#define LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

struct BuildContext;
class IFunctionHandle;

LLVMContext TheContext;
IRBuilder<> Builder(TheContext);

class IFunctionHandle {
public:
  virtual llvm::Function *codegen(BuildContext& context) = 0;
  virtual const std::string& getName() const = 0;
};

struct BuildContext {
  Module* module;
  DIType *DblTy;

  IRBuilder<> IR_builder; 

  DIBuilder* DI_builder;
  DICompileUnit* DI_cunit;
  DIScope *DI_scope;
  DIFile* DI_file;

  DIType *getDoubleTy();

  BuildContext(Module* module, std::string name = "fib"): IR_builder(TheContext) {
    this->module = module;

    this->DI_builder = new DIBuilder(*module);

    this->DI_file = this->DI_builder->createFile("fib.ks", "D:/git/llvm_test/llvm_test");
    this->DI_cunit = this->DI_builder->createCompileUnit(dwarf::DW_LANG_C, this->DI_file, "Kaleidoscope Compiler", 0, "", 0);
    this->DI_scope = this->DI_file;
  }
  void buildFunction(IFunctionHandle*func) {
    if (Function *IR_func = func->codegen(*this)) {
      IR_func->setName(func->getName() + "$impl");
    }
    else report_fatal_error("Couldn't compile lazily JIT'd function");
  }
  void finalize() {
    this->DI_builder->finalize();
  }
};

class SampleJIT {
private:
  std::unique_ptr<TargetMachine> targetMachine;
  const DataLayout dataLayout;

  RTDyldObjectLinkingLayer objectLayer;
  SimpleCompiler moduleCompiler;

  std::unique_ptr<JITCompileCallbackManager> compileCallbackMgr;
  std::unique_ptr<IndirectStubsManager> indirectStubsMgr;

public:
  using ModuleHandle = decltype(objectLayer)::ObjHandleT;

  SampleJIT()
    : targetMachine(EngineBuilder().selectTarget()),
    moduleCompiler(*this->targetMachine),
    dataLayout(this->targetMachine->createDataLayout()),
    objectLayer([]() { return std::make_shared<SectionMemoryManager>(); }),
    compileCallbackMgr(orc::createLocalCompileCallbackManager(this->targetMachine->getTargetTriple(), 0))
  {
    auto IndirectStubsMgrBuilder = orc::createLocalIndirectStubsManagerBuilder(this->targetMachine->getTargetTriple());
    this->indirectStubsMgr = IndirectStubsMgrBuilder();
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }

  TargetMachine &getTargetMachine() {
    return *this->targetMachine;
  }

  Module* createModule(const char* name) {
    Module* TheModule = new Module(name, TheContext);
    TheModule->setDataLayout(this->getTargetMachine().createDataLayout());
    return TheModule;
  }

  ModuleHandle compileModule(Module* M) {
    // Build our symbol resolver:
    // Lambda 1: Look back into the JIT itself to find symbols that are part of
    //           the same "logical dylib".
    // Lambda 2: Search for external symbols in the host process.
    auto Resolver = createLambdaResolver(
      [&](const std::string &Name) {
      if (auto Sym = this->indirectStubsMgr->findStub(Name, false))
        return Sym;
      if (auto Sym = this->objectLayer.findSymbol(Name, false))
        return Sym;
      return JITSymbol(nullptr);
    },
      [](const std::string &Name) {
      if (auto SymAddr =
        RTDyldMemoryManager::getSymbolAddressInProcess(Name))
        return JITSymbol(SymAddr, JITSymbolFlags::Exported);
      return JITSymbol(nullptr);
    });


    // Optimize module
    this->optimizeModule(M);

    // Add the set to the JIT with the resolver we created above and a newly
    // created SectionMemoryManager.
    using tCompileResult = decltype(this->moduleCompiler(*M));
    std::shared_ptr<tCompileResult> Obj = std::make_shared<tCompileResult>(this->moduleCompiler(*M));
    return cantFail(this->objectLayer.addObject(std::move(Obj), std::move(Resolver)));
  }

  Error addFunctionAST(IFunctionHandle* FnAST) {
    // Create a CompileCallback - this is the re-entry point into the compiler
    // for functions that haven't been compiled yet.
    auto CCInfo = this->compileCallbackMgr->getCompileCallback();

    // Create an indirect stub. This serves as the functions "canonical
    // definition" - an unchanging (constant address) entry point to the
    // function implementation.
    // Initially we point the stub's function-pointer at the compile callback
    // that we just created. In the compile action for the callback (see below)
    // we will update the stub's function pointer to point at the function
    // implementation that we just implemented.
    if (auto Err = this->indirectStubsMgr->createStub(
      this->mangle(FnAST->getName()),
      CCInfo.getAddress(),
      JITSymbolFlags::Exported))
    {
      return Err;
    }

    // Move ownership of FnAST to a shared pointer - C++11 lambdas don't support
    // capture-by-move, which is be required for unique_ptr.

    // Set the action to compile our AST. This lambda will be run if/when
    // execution hits the compile callback (via the stub).
    //
    // The steps to compile are:
    // (1) IRGen the function.
    // (2) Add the IR module to the JIT to make it executable like any other
    //     module.
    // (3) Use findSymbol to get the address of the compiled function.
    // (4) Update the stub pointer to point at the implementation so that
    ///    subsequent calls go directly to it and bypass the compiler.
    // (5) Return the address of the implementation: this lambda will actually
    //     be run inside an attempted call to the function, and we need to
    //     continue on to the implementation to complete the attempted call.
    //     The JIT runtime (the resolver block) will use the return address of
    //     this function as the address to continue at once it has reset the
    //     CPU state to what it was immediately before the call.
    CCInfo.setCompileAction([this, FnAST]() {

      BuildContext context(this->createModule("my module"));
      context.buildFunction(FnAST);
      this->compileModule(context.module);
      context.finalize();

      auto Sym = this->findSymbol(FnAST->getName() + "$impl");
      assert(Sym && "Couldn't find compiled function?");

      JITTargetAddress SymAddr = cantFail(Sym.getAddress());
      auto Err = this->indirectStubsMgr->updatePointer(this->mangle(FnAST->getName()), SymAddr);
      if (Err) {
        logAllUnhandledErrors(std::move(Err), errs(), "Error updating function pointer: ");
        exit(1);
      }

      return SymAddr;
    });

    return Error::success();
  }

  JITSymbol findSymbol(const std::string Name) {
    return this->objectLayer.findSymbol(this->mangle(Name), false);
  }

  void removeModule(ModuleHandle H) {
    cantFail(this->objectLayer.removeObject(H));
  }

private:
  std::string mangle(const std::string &Name) {
    std::string MangledName;
    raw_string_ostream MangledNameStream(MangledName);
    Mangler::getNameWithPrefix(MangledNameStream, Name, dataLayout);
    return MangledNameStream.str();
  }

  void optimizeModule(Module* M) {
    // Create a function pass manager.
    auto FPM = llvm::make_unique<legacy::FunctionPassManager>(M);

    // Add some optimizations.
    FPM->add(createInstructionCombiningPass());
    FPM->add(createReassociatePass());
    FPM->add(createGVNPass());
    FPM->add(createCFGSimplificationPass());
    FPM->doInitialization();

    // Run the optimizations over all functions in the module being added to
    // the JIT.
    for (auto &F : *M)
      FPM->run(F);
  }
};


#endif // LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H

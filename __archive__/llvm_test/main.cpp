#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "chrono.h"

#include "SampleJIT.h"
static std::unique_ptr<SampleJIT> TheJIT;

/// FunctionAST - This class represents a function definition itself.
class FunctionAST : public IFunctionHandle {
public:
  std::string name;
  double argValue;
  FunctionAST(std::string name, double argValue) { this->name = name; this->argValue = argValue; }
  virtual const std::string& getName() const { return this->name; }
  virtual llvm::Function *codegen(BuildContext& context) override;
};

class FibFunctionAST : public IFunctionHandle {
public:
  std::string name;
  FibFunctionAST(std::string _name) { this->name = _name; }
  virtual const std::string& getName() const { return this->name; }
  virtual llvm::Function *codegen(BuildContext& context) override;
};

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

Function *FunctionAST::codegen(BuildContext& context) {

  // Transfer ownership of the prototype to the FunctionProtos map, but keep a
  // reference to it for use below.
  FunctionType *FT = FunctionType::get(Type::getDoubleTy(TheContext), false);

  Function *TheFunction = Function::Create(FT, Function::ExternalLinkage, this->getName(), context.module);
  if (!TheFunction) return nullptr;

  // Create a new basic block to start insertion into.
  BasicBlock *BB = BasicBlock::Create(TheContext, "entry", TheFunction);
  Builder.SetInsertPoint(BB);

  std::vector<Type *> fib_Doubles(1, Type::getDoubleTy(TheContext));
  FunctionType *fib_FT = FunctionType::get(Type::getDoubleTy(TheContext), fib_Doubles, false);
  Function *fib = Function::Create(fib_FT, Function::ExternalLinkage, "fib", context.module);

  std::vector<Value *> ArgsV;
  Value *cValue = ConstantFP::get(Type::getDoubleTy(TheContext), this->argValue);
  ArgsV.push_back(cValue);

  Value *RetVal = Builder.CreateCall(fib, ArgsV, "calltmp");

  // Finish off the function.
  Builder.CreateRet(RetVal);

  // Validate the generated code, checking for consistency.
  verifyFunction(*TheFunction);

  return TheFunction;
}

class MDCustomLocation : public MDNode {

};

Function *FibFunctionAST::codegen(BuildContext& context) {
  // Transfer ownership of the prototype to the FunctionProtos map, but keep a
  // reference to it for use below.
  std::vector<Type *> Doubles(1, Type::getDoubleTy(TheContext));
  FunctionType *FT = FunctionType::get(Type::getDoubleTy(TheContext), Doubles, false);

  Function *TheFunction = Function::Create(FT, Function::ExternalLinkage, this->getName() + "$impl", context.module);
  if (!TheFunction) return nullptr;



  SmallVector<Metadata *, 8> EltTys;
  DIType *DI_DblTy = context.DI_builder->createBasicType("double", 64, dwarf::DW_ATE_float);
  EltTys.push_back(DI_DblTy);
  EltTys.push_back(DI_DblTy);
  DISubroutineType* DI_func_type = context.DI_builder->createSubroutineType(context.DI_builder->getOrCreateTypeArray(EltTys));


  DISubprogram* DI_func = context.DI_builder->createFunction(context.DI_scope, "fib", StringRef(), context.DI_file, 42, DI_func_type,
    false /* internal linkage */, true /* definition */, 42, DINode::FlagPrototyped, false);
  TheFunction->setSubprogram(DI_func);

  // Create a new basic block to start insertion into.
  BasicBlock *BB = BasicBlock::Create(TheContext, "entry", TheFunction);
  Builder.SetInsertPoint(BB);

  // Get pointer to the integer argument of the add1 function...
  Value *ArgX = &*TheFunction->arg_begin();// &*TheFunction->arg_begin(); // Get the arg.
  ArgX->setName("x");

  // Get pointers to the constants.
  Value *One = ConstantFP::get(Type::getDoubleTy(TheContext), 1);
  Value *Two = ConstantFP::get(Type::getDoubleTy(TheContext), 2);

  // Create the true_block.
  BasicBlock *RetBB = BasicBlock::Create(TheContext, "return", TheFunction);
  // Create an exit block.
  BasicBlock* RecurseBB = BasicBlock::Create(TheContext, "recurse", TheFunction);

  // Create the "if (arg <= 2) goto exitbb"
  Instruction *CondInst = new FCmpInst(*BB, FCmpInst::FCMP_OLE, ArgX, Two, "cond");
  BranchInst::Create(RetBB, RecurseBB, CondInst, BB);


  CondInst->setDebugLoc(DebugLoc::get(42, 10, DI_func));

  // Create: ret int 1
  ReturnInst::Create(TheContext, One, RetBB);

  // create fib(x-1)
  Instruction *Sub = BinaryOperator::CreateFSub(ArgX, One, "arg", RecurseBB);
  CallInst *CallFibX1 = CallInst::Create(TheFunction, Sub, "fibx1", RecurseBB);
  CallFibX1->setTailCall();

  // create fib(x-2)
  Sub = BinaryOperator::CreateFSub(ArgX, Two, "arg", RecurseBB);
  CallInst *CallFibX2 = CallInst::Create(TheFunction, Sub, "fibx2", RecurseBB);
  CallFibX2->setTailCall();

  // fib(x-1)+fib(x-2)
  Instruction *Sum = BinaryOperator::CreateFAdd(CallFibX1, CallFibX2,
    "addresult", RecurseBB);

  // Create the return instruction and add it to the basic block
  ReturnInst::Create(TheContext, Sum, RecurseBB);

  // Validate the generated code, checking for consistency.
  verifyFunction(*TheFunction);

  return TheFunction;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

struct VFIB {
  static double fib_(double x) {
    if (x < 3)
      return 1;
    else
      return fib_(x - 1) + fib_(x - 2);
  }
  virtual double fib(double x) {
    return fib_(x);
  };
};

VFIB* fib_test = new VFIB();

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  TheJIT = llvm::make_unique<SampleJIT>();

  // Run the main "interpreter loop" now.
  FibFunctionAST* fib_AST = new FibFunctionAST("fib");
  TheJIT->addFunctionAST(fib_AST);


  FunctionAST* test_AST = new FunctionAST("__anon_expr", 40);
  Module* test_Module = TheJIT->createModule("anon module");
  BuildContext test_build(test_Module);
  test_AST->codegen(test_build);
  auto H = TheJIT->compileModule(test_Module);

  // Search the JIT for the __anon_expr symbol.
  JITSymbol test_Sym = TheJIT->findSymbol("__anon_expr");
  double(*test)() = (double(*)())(intptr_t)cantFail(test_Sym.getAddress());
  assert(test && "Function not found");


  Chrono c;
  c.Start();
  double r0 = test();
  printf("time %g\n", c.GetDiffFloat(Chrono::MS));

  c.Start();
  double r1 = test();
  printf("time %g\n", c.GetDiffFloat(Chrono::MS));

  c.Start();
  double r2 = fib_test->fib(40);
  printf("time %g\n", c.GetDiffFloat(Chrono::MS));


  auto fib_Sym = TheJIT->findSymbol("fib$impl");
  double(*fib)(double) = (double(*)(double))(intptr_t)cantFail(fib_Sym.getAddress());

  printf("=> %lg, %lg\n", fib(40), r1);

  getchar();
  return 0;
}

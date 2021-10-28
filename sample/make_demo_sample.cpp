
#include "./headers.h"
#include <Psapi.h>
#include <dbghelp.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "Dbghelp.lib")

using namespace llvm;
using namespace llvm::orc;

BOOL UpdateModulesList() {
   const HANDLE hProcess = GetCurrentProcess();
   const SIZE_T TTBUFLEN = 8096;
   int moduleCount = 0;

   DWORD cbNeeded;
   if (EnumProcessModules(hProcess, 0, 0, &cbNeeded)) {
      HMODULE* hMods = (HMODULE*)malloc(sizeof(HMODULE) * cbNeeded);
      char* imageFileName = (char*)malloc(sizeof(char) * TTBUFLEN);
      char* moduleName = (char*)malloc(sizeof(char) * TTBUFLEN);

      if (EnumProcessModules(hProcess, hMods, cbNeeded, &cbNeeded)) {
         for (int i = 0; i < cbNeeded / sizeof(hMods[0]); i++) {
            MODULEINFO moduleInfo;
            GetModuleInformation(hProcess, hMods[i], &moduleInfo, sizeof(moduleInfo));
            imageFileName[0] = 0;
            GetModuleFileNameExA(hProcess, hMods[i], imageFileName, TTBUFLEN);
            moduleName[0] = 0;
            GetModuleBaseNameA(hProcess, hMods[i], moduleName, TTBUFLEN);

            SymLoadModule64(GetCurrentProcess(), 0, imageFileName, moduleName, (DWORD64)moduleInfo.lpBaseOfDll, moduleInfo.SizeOfImage);
            moduleCount++;
         }
      }
      if (moduleName) free(moduleName);
      if (imageFileName) free(imageFileName);
      if (hMods) free(hMods);
   }
   return moduleCount > 0;
}

void printStack() {
   //UpdateModulesList();
   SymRefreshModuleList(GetCurrentProcess());

#if defined(_WIN64)
   CONTEXT context;
   memset(&context, 0, sizeof(CONTEXT));
   context.ContextFlags = CONTEXT_FULL;
   context.Rsp = (DWORD64)_AddressOfReturnAddress();
   if (context.Rip == 0 && context.Rsp != 0) {
      context.Rip = (ULONG64)(*(PULONG64)context.Rsp);
      context.Rsp += 8;
   }
   for (int i = 0; i < 4096 && context.Rip; i++)
   {
      DWORD64 ImageBase, BaseAddress, InsAddress;
      PRUNTIME_FUNCTION pFunctionEntry = ::RtlLookupFunctionEntry(context.Rip, &ImageBase, NULL);
      if (pFunctionEntry) {
         PVOID HandlerData;
         DWORD64 EstablisherFrame;
         InsAddress = context.Rip;
         BaseAddress = ImageBase + pFunctionEntry->BeginAddress;
         ::RtlVirtualUnwind(UNW_FLAG_NHANDLER,
            ImageBase,
            InsAddress,
            pFunctionEntry,
            &context,
            &HandlerData,
            &EstablisherFrame,
            NULL);
      }
      else {
         InsAddress = context.Rip;
         BaseAddress = context.Rip;
         context.Rip = (ULONG64)(*(PULONG64)context.Rsp);
         context.Rsp += 8;
      }


      struct tSymbol : IMAGEHLP_SYMBOL64 {
         char SymBytes[1024];
         tSymbol() {
            memset(this, 0, sizeof(IMAGEHLP_SYMBOL64) + 1024);
            SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
            MaxNameLength = 1024;
            Name[0] = 0;
         }
      };
      tSymbol symbol;
      DWORD64 offsetFromSymbol;
      if (SymGetSymFromAddr64(GetCurrentProcess(), InsAddress, &offsetFromSymbol, &symbol)) {
         printf("> %p: %s\n", BaseAddress, symbol.Name);
      }
      else {
         printf("> %p\n", BaseAddress);
      }
   }
#endif
}

DIType* DBGetDoubleTy(DIBuilder* DBuilder) {
   return DBuilder->createBasicType("double", 64, dwarf::DW_ATE_float);
}

static DISubroutineType* DBCreateFunctionType(unsigned NumArgs, DIBuilder* DBuilder) {
   SmallVector<Metadata*, 8> EltTys;
   DIType* DblTy = DBGetDoubleTy(DBuilder);

   // Add the result type.
   EltTys.push_back(DblTy);

   for (unsigned i = 0, e = NumArgs; i != e; ++i)
      EltTys.push_back(DblTy);

   return DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray(EltTys));
}

static Function* CreateFibFunction(const char* Name, Module* M, IRBuilder<>* Builder, DIBuilder* DBuilder, DIFile* DUnit, LLVMContext& Context) {

   // Create the fib function and insert it into module M. This function is said
   // to return an int and take an int parameter.
   FunctionType* FibFTy = FunctionType::get(Type::getInt32Ty(Context), { Type::getInt32Ty(Context) }, false);
   Function* FibF = Function::Create(FibFTy, Function::ExternalLinkage, Name, M);

   // >>> Debug Info
   int LineNo = 1;
   int ScopeLine = 2;
   DISubprogram* SP = DBuilder->createFunction(
      DUnit, FibF->getName(), StringRef(), DUnit, LineNo,
      DBCreateFunctionType(FibF->arg_size(), DBuilder), ScopeLine,
      DINode::FlagPrototyped, DISubprogram::SPFlagDefinition);
   FibF->setSubprogram(SP);

   // Add a basic block to the function.
   BasicBlock* BB = BasicBlock::Create(Context, "EntryBlock", FibF);
   Builder->SetInsertPoint(BB);

   // Get pointers to the constants.
   Value* One = ConstantInt::get(Type::getInt32Ty(Context), 1);
   Value* Two = ConstantInt::get(Type::getInt32Ty(Context), 2);

   // Get pointer to the integer argument of the add1 function...
   Argument* ArgX = &*FibF->arg_begin(); // Get the arg.
   ArgX->setName("AnArg");            // Give it a nice symbolic name for fun.

   // Create the true_block.
   BasicBlock* RetBB = BasicBlock::Create(Context, "return", FibF);

   // Create an exit block.
   BasicBlock* RecurseBB = BasicBlock::Create(Context, "recurse", FibF);

   // Create the "if (arg <= 2) goto exitbb"
   auto CondInst = new ICmpInst(*BB, ICmpInst::ICMP_SLE, ArgX, Two, "cond");
   CondInst->setDebugLoc(DILocation::get(Context, 6, 0, SP));
   auto Br1 = BranchInst::Create(RetBB, RecurseBB, CondInst, BB);
   Br1->setDebugLoc(DILocation::get(Context, 7, 0, SP));

   // Create: ret int 1
   auto Ret1 = ReturnInst::Create(Context, One, RetBB);
   Ret1->setDebugLoc(DILocation::get(Context, 6, 0, SP));

   // create fib(x-1)
   auto Sub = BinaryOperator::CreateSub(ArgX, One, "arg", RecurseBB);
   Sub->setDebugLoc(DILocation::get(Context, 1, 0, SP));

   auto CallFibX1 = CallInst::Create(FibF, Sub, "fibx1", RecurseBB);
   CallFibX1->setTailCall();
   CallFibX1->setDebugLoc(DILocation::get(Context, 2, 0, SP));

   // create fib(x-2)
   Sub = BinaryOperator::CreateSub(ArgX, Two, "arg", RecurseBB);
   Sub->setDebugLoc(DILocation::get(Context, 3, 0, SP));

   auto CallFibX2 = CallInst::Create(FibF, Sub, "fibx2", RecurseBB);
   CallFibX2->setTailCall();
   CallFibX2->setDebugLoc(DILocation::get(Context, 3, 0, SP));

   // fib(x-1)+fib(x-2)
   auto Sum = BinaryOperator::CreateAdd(CallFibX1, CallFibX2, "addresult", RecurseBB);
   Sum->setDebugLoc(DILocation::get(Context, 4, 0, SP));

   // call doSnapshot()
   auto doSnapshotF = M->getOrInsertFunction("doSnapshot", Type::getVoidTy(Context));
   auto CallSnapshot = CallInst::Create(doSnapshotF, {}, Twine(), RecurseBB);

   // Create the return instruction and add it to the basic block
   auto Ret2 = ReturnInst::Create(Context, Sum, RecurseBB);
   Ret2->setDebugLoc(DILocation::get(Context, 5, 0, SP));

   return FibF;
}

StringRef sourceFile = R"(
ThreadSafeModule createDemoModule() {
   auto Context = std::make_unique<LLVMContext>();
   auto M = std::make_unique<Module>("test", *Context);

   auto Builder = std::make_unique<IRBuilder<>>(*Context);
   auto DBuilder = std::make_unique<DIBuilder>(*M);

   DIFile* DUnit = DBuilder->createFile("test.txt", "d:/sources");
   DBuilder->createCompileUnit(dwarf::DW_LANG_C, DUnit, "Kaleidoscope Compiler", 0, "", 0);

   CreateFibFunction(M.get(), Builder.get(), DBuilder.get(), DUnit, *Context);

   DBuilder->finalize();
   return ThreadSafeModule(std::move(M), std::move(Context));
}
)";

ThreadSafeModule createDemoModule(LLJIT* J) {
   auto Context = std::make_unique<LLVMContext>();
   auto M = std::make_unique<Module>("test", *Context);
   //M->addModuleFlag(llvm::Module::Warning, "CodeView", 1);

   auto Builder = std::make_unique<IRBuilder<>>(*Context);
   auto DBuilder = std::make_unique<DIBuilder>(*M);

   DIFile* DUnit = DBuilder->createFile("test.txt", "d:/sources", NoneType::None, sourceFile);
   DBuilder->createCompileUnit(dwarf::DW_LANG_C, DUnit, "Kaleidoscope Compiler", 0, "", 0);

   //CreateFibFunction("fib2", M.get(), Builder.get(), DBuilder.get(), DUnit, *Context);
   CreateFibFunction("fib", M.get(), Builder.get(), DBuilder.get(), DUnit, *Context);

   DBuilder->finalize();

   return ThreadSafeModule(std::move(M), std::move(Context));
}

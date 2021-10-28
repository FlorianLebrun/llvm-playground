#include "headers.h"

#include <Psapi.h>
#include <dbghelp.h>
#undef IMAGE_SCN_LNK_COMDAT

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "Dbghelp.lib")

using namespace llvm;
using namespace llvm::orc;

using ObjFormat = Triple::ObjectFormatType;
ExitOnError ExitOnErr;

JITEventListener* LLVMCreateGDBRegistrationListener2();
void createDemoModule(Module& M);
ThreadSafeModule createDemoJITModule(LLJIT* J);
void printStack();

extern"C" void doSnapshot() {

   printf("\n------ snapshot ------\n");
   printStack();
   //   while (1);
}

void printObject(const llvm::object::ObjectFile& Object, const llvm::RuntimeDyld::LoadedObjectInfo* LOS) {

   printf("\nobject %s: %d bytes @%p\n",
      Object.getFileFormatName().data(),
      Object.getData().size(),
      Object.getData().data()
   );

   uintptr_t RangeBase = 0, RangeEnd = 0;
   uintptr_t EHFramePtr = 0;
   for (const object::SectionRef& lSection : Object.sections()) {
      auto sName = *lSection.getName();
      if (!LOS) {
         printf("section RVA=%p [%d bytes] %s\n",
            lSection.getRawDataRefImpl(),
            lSection.getSize(),
            sName.data()
         );
      }
      else if (auto VA = LOS->getSectionLoadAddress(lSection)) {
         printf("section VA=%p RVA=%p [%d bytes] %s\n",
            VA,
            lSection.getRawDataRefImpl(),
            lSection.getSize(),
            sName.data()
         );
      }
   }

   for (auto& sym : Object.symbols()) {
      const char* type = "";
      switch (sym.getType().get()) {
      case object::SymbolRef::ST_Unknown: type = "ST_Unknown"; break;
      case object::SymbolRef::ST_Data: type = "ST_Data"; break;
      case object::SymbolRef::ST_Debug: type = "ST_Debug"; break;
      case object::SymbolRef::ST_File: type = "ST_File"; break;
      case object::SymbolRef::ST_Function: type = "ST_Function"; break;
      case object::SymbolRef::ST_Other: type = "ST_Other"; break;
      }
      printf("symbol %s: %s = %p\n",
         sym.getName()->data(),
         type,
         sym.getValue().get()
      );
   }
}

#include "./ModuleManager/ModuleManager.h"
#include "./ExecutionEngine/RTObjectLinkingLayer.h"
#include "./ExecutionEngine/RTExecutionEngine.h"


//extern"C" int fib(int);

int main(int argc, char* argv[]) {
   printf("Process: %d\n\n", GetCurrentProcessId());

   if (!SymInitialize(GetCurrentProcess(), 0, FALSE)) throw "Cannot init symbols";

   DWORD symOptions = SymGetOptions();
   symOptions |= SYMOPT_LOAD_LINES;
   symOptions |= SYMOPT_FAIL_CRITICAL_ERRORS;
   symOptions |= SYMOPT_NO_PROMPTS;
   symOptions = SymSetOptions(symOptions);

   InitLLVM X(argc, argv);
   InitializeNativeTarget();
   InitializeNativeTargetAsmPrinter();

   RTExecutionEngine exec(ObjFormat::COFF);
   //RTExecutionEngine exec(ObjFormat::ELF);
   exec.addAbsoluteSymbol("doSnapshot", doSnapshot);

   if (1) {
      ModuleManager modManager(ObjFormat::COFF);
      auto M = modManager.createModule("test");
      createDemoModule(*M);
      auto ObjMem = modManager.makeModule(std::unique_ptr<ManagedModule>(M));
      exec.addObjectFile(std::move(ObjMem));
   }
   else {
      auto M = createDemoJITModule(exec.JIT.get());
      exec.addModule(std::move(M));
   }

   //exec.JIT->getMainJITDylib().dump(raw_fd_ostream(1, false));

   try {
      // Look up the JIT'd function, cast it to a function pointer, then call it.
      void (*doSnapshotF)() = (void (*)(void))exec.getSymbolAddress("doSnapshot")->getAddress();
      int (*fibF)(int) = (int (*)(int))exec.getSymbolAddress("fib")->getAddress();
      int (*fib2F)(int) = (int (*)(int))exec.getSymbolAddress("fib2")->getAddress();
      int n = 4;
      int Result;
      Result = fibF(n);
      Result = fib2F(n);
      outs() << "fib(" << n << ") = " << Result << "\n";
   }
   catch (std::exception e) {
      printf("exception: %s\n", e.what());
   }
   catch (...) {
      printf("exception: *\n");
   }
   return 0;
}

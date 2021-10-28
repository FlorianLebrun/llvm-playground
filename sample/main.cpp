
#include "./headers.h"
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/IR/LegacyPassManager.h>
#include <iostream>

#include <windows.h>
#include <Psapi.h>
#include <dbghelp.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "Dbghelp.lib")

using namespace llvm;
using namespace llvm::orc;

ExitOnError ExitOnErr;

ThreadSafeModule createDemoModule(LLJIT* J);
void printStack();

extern"C" void doSnapshot() {

   printf("\n------ snapshot ------\n");
   printStack();
   while (1);
}

class RTModuleCompiler : public IRCompileLayer::IRCompiler {
private:
   TargetMachine& TM;
   std::string cacheDir;
public:
   using CompileResult = std::unique_ptr<MemoryBuffer>;

   /// Construct a simple compile functor with the given target.
   RTModuleCompiler(TargetMachine& TM, const char* cacheDir)
      : IRCompiler(orc::irManglingOptionsFromTargetOptions(TM.Options)), TM(TM), cacheDir(cacheDir) {
   }

   ~RTModuleCompiler() override {

   }

   /// Compile a Module to an ObjectFile.
   Expected<CompileResult> operator()(Module& M) override {

      CompileResult CachedObject = this->readPrecompiledObject(&M);
      if (CachedObject) {
         return std::move(CachedObject);
      }

      SmallVector<char, 0> ObjBufferSV;
      {
         raw_svector_ostream ObjStream(ObjBufferSV);

         legacy::PassManager PM;
         MCContext* Ctx = 0;
         if (TM.addPassesToEmitMC(PM, Ctx, ObjStream)) {
            return make_error<StringError>("Target does not support MC emission",
               inconvertibleErrorCode());
         }
         PM.run(M);
      }

      auto ObjBuffer = std::make_unique<SmallVectorMemoryBuffer>(
         std::move(ObjBufferSV), M.getModuleIdentifier() + "-jitted-objectbuffer");

      auto Obj = object::ObjectFile::createObjectFile(ObjBuffer->getMemBufferRef());
      if (!Obj) {
         return Obj.takeError();
      }

      this->writePrecompiledObject(&M, *ObjBuffer);
      return std::move(ObjBuffer);
   }

   void writePrecompiledObject(const Module* M, MemoryBufferRef Obj) {
      auto filename = this->getModuleFilename(M);
      while (sys::fs::exists(filename)) {
         sys::fs::remove(filename);
      }
      std::error_code Err;
      raw_fd_ostream OStream(filename, Err);
      if (!Err) {
         OStream.write(Obj.getBufferStart(), Obj.getBufferSize());
      }
      else {
         dbgs() << "Cannot write object for " << filename << " in cache.\n";
      }
   }
   std::unique_ptr<MemoryBuffer> readPrecompiledObject(const Module* M) {
      auto filename = this->getModuleFilename(M);
      auto _Result = MemoryBuffer::getFile(filename);
      if (_Result) {
         auto bin = _Result->get();
         if (0 && !_Result.getError()) {
            dbgs() << "Object for " << filename << " loaded from cache.\n";
            return std::move(*_Result);
         }
      }
      dbgs() << "No object for " << filename << " in cache. Compiling.\n";
      return nullptr;
   }
   std::string getModuleFilename(const Module* M) {
      std::string filename;
      raw_string_ostream(filename) << this->cacheDir << "/" << M->getModuleIdentifier() << ".obj";
      return filename;
   }
};

// A simple forwarding class, since OrcJIT v2 needs a unique_ptr, while we have a shared_ptr
class ForwardingMemoryManager : public RuntimeDyld::MemoryManager {
private:
   std::unique_ptr<RuntimeDyld::MemoryManager> MemMgr;

public:
   ForwardingMemoryManager(RuntimeDyld::MemoryManager* MemMgr) : MemMgr(MemMgr) {}
   virtual ~ForwardingMemoryManager() = default;

   virtual uint8_t* allocateCodeSection(uintptr_t Size, unsigned Alignment,
      unsigned SectionID, StringRef SectionName) override
   {
      return MemMgr->allocateCodeSection(Size, Alignment, SectionID, SectionName);
   }
   virtual uint8_t* allocateDataSection(uintptr_t Size, unsigned Alignment,
      unsigned SectionID, StringRef SectionName, bool IsReadOnly) override
   {
      return MemMgr->allocateDataSection(Size, Alignment, SectionID, SectionName, IsReadOnly);
   }
   virtual void reserveAllocationSpace(uintptr_t CodeSize, uint32_t CodeAlign,
      uintptr_t RODataSize, uint32_t RODataAlign,
      uintptr_t RWDataSize, uint32_t RWDataAlign) override
   {
      return MemMgr->reserveAllocationSpace(CodeSize, CodeAlign, RODataSize, RODataAlign, RWDataSize, RWDataAlign);
   }
   virtual bool needsToReserveAllocationSpace() override {
      return MemMgr->needsToReserveAllocationSpace();
   }
   virtual void registerEHFrames(uint8_t* Addr, uint64_t LoadAddr, size_t Size) override {
      struct tCIEField {
         uint32_t Length;// 	4 bytes	Total length of the CIE except this field
         uint32_t CIE_id;// 	4 or 8 bytes	0 for .eh_frame
         uint8_t Version;// 	1 byte	Value 1
         uint8_t Augmentation[3];// 	A null-terminated UTF-8 string	0 if no augmetation
         uint8_t CodeAlignmentFactor;// 	unsigned LEB128	Usually 1
         uint8_t DataAlignmentFactor;// 	signed LEB128	Usually -4 (encoded as 0x7C)
         uint8_t ReturnAddressRegister;// 	unsigned LEB128	Dwarf number of the return register
         uint8_t AugmentationDataLength;// 	unsigned LEB128	Present if Augmentation has �z�
         uint8_t InitialInstructions;//	array of bytes	Dwarf Call Frame Instructions
      };
      struct tFDEField {
         uint32_t Length;// 	4 bytes	Total length of the FDE except this field; 0 means end of all records
         uint32_t CIE_pointer;// 	4 or 8 bytes	Distance to the nearest preceding(parent) CIE
         uint32_t InitialLocation;// 	various bytes	Reference to the function corresponding to the FDE
         uint32_t RangeLength;// 	various bytes	Size of the function corresponding to the FDE
         uint8_t AugmentationDataLength;// 	unsigned LEB128	Present if CIE Augmentation is non - empty
         uint32_t Instructions;//	array of bytes	Dwarf Call Frame Instructions
      };
      uint8_t* bytes = (uint8_t*)Addr;
      tCIEField* cie = (tCIEField*)bytes; bytes += cie->Length;
      tFDEField* fde = (tFDEField*)bytes; bytes += fde->Length;
      /*if (!RtlAddFunctionTable(PRUNTIME_FUNCTION(Addr), 1, DWORD64(LoadAddr))) {
         printf("EH frame mis registered !!!\n");
      }*/
      return MemMgr->registerEHFrames(Addr, LoadAddr, Size);
   }
   virtual void deregisterEHFrames() override {
      return MemMgr->deregisterEHFrames();
   }
   virtual bool finalizeMemory(std::string* ErrMsg = nullptr) override {
      return MemMgr->finalizeMemory(ErrMsg);
   }
   virtual void notifyObjectLoaded(RuntimeDyld& RTDyld, const object::ObjectFile& Obj) override {
      return MemMgr->notifyObjectLoaded(RTDyld, Obj);
   }
};

class RTExecutionEngine {
public:
   std::unique_ptr<LLJIT> JIT;
   RTModuleCompiler* Compiler = 0;

   RTExecutionEngine() {
      LLJITBuilder JBuilder;
      auto JTMB = ExitOnErr(JITTargetMachineBuilder::detectHost());
      // JTMB.getTargetTriple().setObjectFormat(Triple::ObjectFormatType::ELF);
      // JTMB.getOptions().ExceptionModel = ExceptionHandling::WinEH;
      // JTMB.getOptions().WinEHEncodingType = ExceptionHandling::WinEH;

      // Create a LLJIT builder & instance
      JBuilder.setJITTargetMachineBuilder(JTMB);
      JBuilder.setCompileFunctionCreator([this](auto JTMB) {
         return this->createModuleCompiler(JTMB);
         });
      JBuilder.setObjectLinkingLayerCreator([this](auto& ES, auto& T) {
         return this->createObjectLinker(ES, T);
         });
      this->JIT = ExitOnErr(JBuilder.create());

      auto triple = JIT->getTargetTriple();

      auto& ES = this->JIT->getExecutionSession();
      auto& DL = this->JIT->getMainJITDylib();

      // Declare public symbols from engine
      ExitOnErr(DL.define(orc::absoluteSymbols(
         {
            {
               this->JIT->mangleAndIntern("doSnapshot"),
               JITEvaluatedSymbol::fromPointer(doSnapshot),
            },
         }
      )));

      // Handle 'Error': manage error logging
      // TODO: ES.setErrorReporter([](Error err) {      printf("error\n");      });

   }
   void addModule(ThreadSafeModule M) {
      this->JIT->addIRModule(std::move(M));
      // ExitOnErr(this->J->getIRCompileLayer().add(*this->JD, std::move(M)));
   }
   JITTargetAddress getSymbolAddress(const char* name) {
      auto& ES = this->JIT->getExecutionSession();
      auto& DL = this->JIT->getMainJITDylib();
      auto sym = ExitOnErr(ES.lookup({ &DL }, name));
      return sym.getAddress();
   }
private:

   Expected<std::unique_ptr<IRCompileLayer::IRCompiler>>
      createModuleCompiler(JITTargetMachineBuilder& JTMB)
   {
      auto TM = ExitOnErr(JTMB.createTargetMachine());
      this->Compiler = new RTModuleCompiler(*TM.release(), "d:/dump");
      return std::unique_ptr<IRCompileLayer::IRCompiler>(this->Compiler);
   }

   Expected<std::unique_ptr<ObjectLayer>>
      createObjectLinker(ExecutionSession& ES, const Triple& T)
   {
      // Otherwise default to creating an RTDyldObjectLinkingLayer that constructs
      // a new SectionMemoryManager for each object.
      auto GetMemMgr = []() -> std::unique_ptr<RuntimeDyld::MemoryManager> {
         return std::make_unique<ForwardingMemoryManager>(new SectionMemoryManager());
      };
      auto ObjLinkingLayer = std::make_unique<RTDyldObjectLinkingLayer>(ES, std::move(GetMemMgr));

      if (T.isOSBinFormatCOFF()) {
         ObjLinkingLayer->setOverrideObjectFlagsWithResponsibilityFlags(true);
         ObjLinkingLayer->setAutoClaimResponsibilityForObjectSymbols(true);
      }
      ObjLinkingLayer->setProcessAllSections(true);
      ObjLinkingLayer->registerJITEventListener(*JITEventListener::createGDBRegistrationListener());

      // Handle 'when object sections are linked in memory': register EH Frames
      ObjLinkingLayer->setNotifyLoaded(
         [this](orc::MaterializationResponsibility& MR, const object::ObjectFile& Object, const RuntimeDyld::LoadedObjectInfo& LOS) {
            this->NotifyObjectEmitted(Object, LOS);
         });

      return std::unique_ptr<ObjectLayer>(std::move(ObjLinkingLayer));
   }

   virtual void NotifyObjectEmitted(const object::ObjectFile& Object,
      const RuntimeDyld::LoadedObjectInfo& LOS)
   {
      printf("\nobject %s: %d bytes @%p\n", Object.getFileFormatName().data(), Object.getData().size(), Object.getData().data());

      if (!LOS.getObjectForDebug(Object).getBinary()) {
         printf("> debug info missing !!!\n");
      }

      // Register COFF EH frames data
      //--- Find function table adresses
      uintptr_t RangeBase = 0, RangeEnd = 0;
      uintptr_t EHFramePtr = 0;
      for (const object::SectionRef& lSection : Object.sections()) {
         auto sName = *lSection.getName();
         printf("section '%s': %p [%d bytes @%p]\n", sName.data(), LOS.getSectionLoadAddress(lSection), lSection.getSize(), lSection.getRawDataRefImpl());
         if (sName == ".text") {
            RangeBase = LOS.getSectionLoadAddress(lSection);
            RangeEnd = RangeBase + lSection.getSize();
         }
         else if (sName == ".pdata") {
            EHFramePtr = LOS.getSectionLoadAddress(lSection);
         }
      }
      printf("eh_frame [%p - %p]: %p\n", RangeBase, RangeEnd, EHFramePtr);
      //--- Register function table
      if (!RtlAddFunctionTable(PRUNTIME_FUNCTION(EHFramePtr), 1, RangeBase)) {
         printf("EH frame mis registered !!!\n");
      }

      auto symbols = object::computeSymbolSizes(Object);
      for (const auto& sym_kv : symbols) {
         auto sym = sym_kv.first;
         auto lSection = *ExitOnErr(sym.getSection());
         //         printf("symbols '%s': %p [%d bytes @%p]\n", sym.getName()->data(), LOS.getSectionLoadAddress(lSection), lSection.getSize(), lSection.getRawDataRefImpl());
         if (sym.getType().get() != object::SymbolRef::ST_Function) continue;
         auto BaseAddr = LOS.getSectionLoadAddress(*sym.getSection().get());
         auto Addr = sym.getAddress().get();
         auto Size = sym_kv.second;
         if (!SymAddSymbol(GetCurrentProcess(), (ULONG64)BaseAddr, sym.getName().get().data(),
            (DWORD64)Addr, (DWORD)Size, 0)) {
            printf("WARNING: failed to insert function name '%s' into debug info: %lu\n", sym.getName().get().data(), GetLastError());
         }
      }
   }
};

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

   RTExecutionEngine exec;

   //fib(4);

   auto M = createDemoModule(exec.JIT.get());
   exec.addModule(std::move(M));

   try {
      // Look up the JIT'd function, cast it to a function pointer, then call it.
      int (*fibF)(int) = (int (*)(int))exec.getSymbolAddress("fib");
      int n = 4;
      int Result = fibF(n);
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

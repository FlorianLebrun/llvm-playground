
// A simple forwarding class, since OrcJIT v2 needs a unique_ptr, while we have a shared_ptr
class RTExecutionMemoryManager : public RuntimeDyld::MemoryManager {
private:
   std::unique_ptr<RuntimeDyld::MemoryManager> MemMgr;
   SectionMemoryManager sectionManager;

public:
   RTExecutionMemoryManager() : MemMgr(new SectionMemoryManager()) {}
   virtual ~RTExecutionMemoryManager() = default;

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
      printf("registerEHFrames [%p - %d]: %p\n", LoadAddr, Size, Addr);
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

class RTEHFrameRegister : public JITEventListener {

   virtual void notifyObjectLoaded(ObjectKey K, const object::ObjectFile& Object,
      const RuntimeDyld::LoadedObjectInfo& LOS) {

      auto m = Object.getMemoryBufferRef();
      std::ofstream("d:/obj1.o", std::ios::out).write(m.getBufferStart(), m.getBufferSize());


      if (!LOS.getObjectForDebug(Object).getBinary()) {
         printf("> debug info missing !!!\n");
      }
      printObject(Object, &LOS);

      // Register COFF EH frames data
      //--- Find function table adresses
      uintptr_t RangeBase = 0, RangeEnd = 0;
      PRUNTIME_FUNCTION EHFrameTable = 0;
      uintptr_t EHFrameCount = 0;
      for (const object::SectionRef& lSection : Object.sections()) {
         auto sName = *lSection.getName();
         if (sName == ".text") {
            RangeBase = LOS.getSectionLoadAddress(lSection);
            RangeEnd = RangeBase + lSection.getSize();
         }
         else if (sName == ".pdata") {
            EHFrameTable = (PRUNTIME_FUNCTION)LOS.getSectionLoadAddress(lSection);
            EHFrameCount = lSection.getSize() / sizeof(RUNTIME_FUNCTION);
         }
      }
      //--- Register function table
      if (EHFrameCount) {
         printf("eh_frame [%p - %p]: %p\n", RangeBase, RangeEnd, EHFrameTable);
         if (!RtlAddFunctionTable(&EHFrameTable[0], 2, RangeBase)) {
            printf("EH frame mis registered !!!\n");
         }
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

   virtual void notifyFreeingObject(ObjectKey K) {
   }

};

class RTExecutionEngine {
public:
   std::unique_ptr<LLJIT> JIT;
   RTEHFrameRegister EHFrameRegister;
   ObjFormat format;

   RTExecutionEngine(ObjFormat format) : format(format) {
      LLJITBuilder JBuilder;
      auto JTMB = ExitOnErr(JITTargetMachineBuilder::detectHost());
      JTMB.getTargetTriple().setObjectFormat(format);

      if (format == ObjFormat::ELF) {
         JTMB.getOptions().ExceptionModel = ExceptionHandling::WinEH;
         //JTMB.getOptions().WinEHEncodingType = ExceptionHandling::WinEH;
      }
      printf("machine triple %s\n", JTMB.getTargetTriple().str().data());

      // Create a LLJIT builder & instance
      JBuilder.setJITTargetMachineBuilder(JTMB);
      JBuilder.setCompileFunctionCreator([this](auto JTMB) {
         return this->createModuleCompiler(JTMB);
         });
      JBuilder.setObjectLinkingLayerCreator([this](auto& ES, auto& T) {
         return this->createObjectLinker(ES, T);
         });

      this->JIT = ExitOnErr(JBuilder.create());
   }
   void addObjectFile(std::unique_ptr<MemoryBuffer> obj) {
      auto& ES = this->JIT->getExecutionSession();
      auto& DL = this->JIT->getMainJITDylib();

      auto ObjSymInfo = getObjectSymbolInfo(ES, obj->getMemBufferRef());
      if (!ObjSymInfo) throw;

      auto& SymbolFlags = ObjSymInfo->first;
      auto& InitSymbol = ObjSymInfo->second;

      for (auto& KV : SymbolFlags) {
         JITSymbolFlags& Flags = KV.second;
         Flags |= JITSymbolFlags::Exported;
      }

      auto MU = new BasicObjectLayerMaterializationUnit(
         this->JIT->getObjLinkingLayer(), std::move(obj),
         std::move(SymbolFlags), std::move(InitSymbol)
      );

      DL.define(std::unique_ptr<MaterializationUnit>(MU));
   }
   void addModule(ThreadSafeModule M) {
      this->JIT->addIRModule(std::move(M));
   }
   Expected<JITEvaluatedSymbol> getSymbolAddress(const char* name) {
      auto& ES = this->JIT->getExecutionSession();
      auto& DL = this->JIT->getMainJITDylib();
      return ES.lookup({ &DL }, name);
   }
   void addAbsoluteSymbols(SymbolMap& Symbols) {
      auto& DL = this->JIT->getMainJITDylib();
      DL.define(absoluteSymbols(std::move(Symbols)));
   }
   void addAbsoluteSymbol(const char* name, void* ptr) {
      SymbolMap Symbols;
      Symbols[this->JIT->mangleAndIntern(name)] = JITEvaluatedSymbol::fromPointer(ptr);
      this->addAbsoluteSymbols(Symbols);
   }
private:

   Expected<std::unique_ptr<IRCompileLayer::IRCompiler>>
      createModuleCompiler(JITTargetMachineBuilder& JTMB)
   {
      struct HookCompiler : SimpleCompiler {
         HookCompiler(TargetMachine& TM) : SimpleCompiler(TM) {
         }
         Expected<CompileResult> operator()(Module& M) override {
            printf("compile %s`\n", M.getSourceFileName().c_str());
            return this->SimpleCompiler::operator()(M);
         }
      };
      auto TM = ExitOnErr(JTMB.createTargetMachine());
      auto Compiler = new HookCompiler(*TM.release());
      return std::unique_ptr<IRCompileLayer::IRCompiler>(Compiler);
   }

   Expected<std::unique_ptr<ObjectLayer>>
      createObjectLinker(ExecutionSession& ES, const Triple& T)
   {
      auto GetMemMgr = []() -> std::unique_ptr<RuntimeDyld::MemoryManager> {
         return std::make_unique<RTExecutionMemoryManager>();
      };
      auto ObjLinkingLayer = std::make_unique<RTObjectLinkingLayer>(ES, std::move(GetMemMgr));

      ObjLinkingLayer->EventListeners.push_back(&this->EHFrameRegister);
      ObjLinkingLayer->EventListeners.push_back(LLVMCreateGDBRegistrationListener2());

      return std::unique_ptr<ObjectLayer>(std::move(ObjLinkingLayer));
   }
};

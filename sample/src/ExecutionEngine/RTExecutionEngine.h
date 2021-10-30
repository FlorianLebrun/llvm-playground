
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


class ModuleManager;
class RTModuleCompiler;

class ManagedModule : public Module {
   friend ModuleManager;
   ModuleManager* manager;
   ManagedModule(StringRef ModuleID, LLVMContext* context = new LLVMContext()) :
      Module(ModuleID, *context)
   {
   }
public:
   ModuleManager* getManager() {
      return manager;
   }
};

class ModuleManager {
   using ObjFormat = llvm::Triple::ObjectFormatType;
   RTModuleCompiler* Compiler;
public:
   ModuleManager(ObjFormat format);
   ManagedModule* createModule(StringRef ModuleID);
   std::unique_ptr<MemoryBuffer> makeModule(std::unique_ptr<ManagedModule> M);
};

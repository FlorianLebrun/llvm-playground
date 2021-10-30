#include "headers.h"
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/Host.h>

#include "./ModuleManager.h"
#include "./ModuleCompiler.h"

static Expected<RTModuleCompiler*> createHostModuleCompiler() {
   auto targetTriple = sys::getProcessTriple();

   std::string CPU = std::string(llvm::sys::getHostCPUName());
   SubtargetFeatures Features;
   TargetOptions Options;
   Optional<Reloc::Model> RM;
   Optional<CodeModel::Model> CM;
   CodeGenOpt::Level OptLevel = CodeGenOpt::Default;

   std::string ErrMsg;
   const Target* target = TargetRegistry::lookupTarget(targetTriple, ErrMsg);
   if (!target) {
      return make_error<StringError>(std::move(ErrMsg), inconvertibleErrorCode());
   }

   // Retrieve host CPU name and sub-target features and add them to builder.
   // Relocation model, code model and codegen opt level are kept to default
   // values.
   llvm::StringMap<bool> HostFeatureMap;
   llvm::sys::getHostCPUFeatures(HostFeatureMap);
   for (auto& Feature : HostFeatureMap) {
      Features.AddFeature(Feature.first(), Feature.second);
   }

   TargetMachine* targetMachine = target->createTargetMachine(
      targetTriple,
      llvm::sys::getHostCPUName(),
      Features.getString(),
      Options, RM, CM, OptLevel,
      /*JIT*/ true
   );
   if (!targetMachine) {
      return make_error<StringError>("Could not allocate target machine", inconvertibleErrorCode());
   }

   return new RTModuleCompiler(*targetMachine, "d:/dump");
}

ModuleManager::ModuleManager(ObjFormat format) {
   auto JTMB = ExitOnErr(JITTargetMachineBuilder::detectHost());
   JTMB.getTargetTriple().setObjectFormat(format);
   JTMB.createTargetMachine();

   this->Compiler = createHostModuleCompiler().get();
}

std::unique_ptr<MemoryBuffer> ModuleManager::makeModule(std::unique_ptr<ManagedModule> M) {
   auto context = &M->getContext();
   auto objMem = std::move(this->Compiler->compile(M.get()).get());
   delete M.release();
   delete context;
   if (!objMem) return 0;

   /*std::ofstream("d:/obj2.o", std::ios::out).write(objMem->getBufferStart(), objMem->getBufferSize());
   auto obj = object::ObjectFile::createObjectFile((objMem)->getMemBufferRef());
   if (!obj) return 0;
   printObject(*obj->get());*/

   return std::move(objMem);
}

ManagedModule* ModuleManager::createModule(StringRef ModuleID) {
   auto M = new ManagedModule(ModuleID);
   M->setDataLayout(Compiler->targetMachine.createDataLayout());

   // ExitOnErr(this->J->getIRCompileLayer().add(*this->JD, std::move(M)));
   bool useCodeView = 1;
   if (useCodeView) {
      M->setTargetTriple(Compiler->targetTriple.str());
      M->addModuleFlag(llvm::Module::Warning, "CodeView", 1);
   }
   return M;
}

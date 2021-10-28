#pragma once

class RTModuleCompiler {
public:
   Triple targetTriple;
   TargetMachine& targetMachine;
   std::string cacheDir;

   /// Construct a simple compile functor with the given target.
   RTModuleCompiler(TargetMachine& TM, const char* cacheDir) :
      targetTriple(TM.getTargetTriple()),
      targetMachine(TM),
      cacheDir(cacheDir)
   {
   }
   ~RTModuleCompiler() {

   }

   /// Compile a Module to an ObjectFile.
   Expected<std::unique_ptr<MemoryBuffer>> compile(Module* M) {
      SmallVector<char, 0> ObjBufferSV;

      MCContext* Ctx = 0;
      legacy::PassManager PM;
      raw_svector_ostream ObjStream(ObjBufferSV);
      if (targetMachine.addPassesToEmitMC(PM, Ctx, ObjStream)) {
         return make_error<StringError>("Target does not support MC emission", inconvertibleErrorCode());
      }
      PM.run(*M);

      return std::make_unique<SmallVectorMemoryBuffer>(std::move(ObjBufferSV), M->getModuleIdentifier() + "-jitted-objectbuffer");
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

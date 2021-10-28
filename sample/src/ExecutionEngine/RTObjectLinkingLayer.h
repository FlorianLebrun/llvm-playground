
class RTObjectLinkingLayer : public ObjectLayer, private ResourceManager {
public:

   /// Functor for receiving object-loaded notifications.
   using NotifyLoadedFunction = std::function<void(
      MaterializationResponsibility& R, const object::ObjectFile& Obj,
      const RuntimeDyld::LoadedObjectInfo&)>;

   /// Functor for receiving finalization notifications.
   using NotifyEmittedFunction = std::function<void(
      MaterializationResponsibility& R, std::unique_ptr<MemoryBuffer>)>;

   using GetMemoryManagerFunction =
      std::function<std::unique_ptr<RuntimeDyld::MemoryManager>()>;

   /// Construct an ObjectLinkingLayer with the given NotifyLoaded,
   ///        and NotifyEmitted functors.
   RTObjectLinkingLayer(ExecutionSession& ES,
      GetMemoryManagerFunction GetMemoryManager);

   ~RTObjectLinkingLayer();

   /// Emit the object.
   void emit(std::unique_ptr<MaterializationResponsibility> R,
      std::unique_ptr<MemoryBuffer> O) override;


   bool ProcessAllSections = false;

   std::vector<JITEventListener*> EventListeners;
   GetMemoryManagerFunction GetMemoryManager;
private:
   using MemoryManagerUP = std::unique_ptr<RuntimeDyld::MemoryManager>;

   Error onObjLoad(MaterializationResponsibility& R,
      const object::ObjectFile& Obj,
      RuntimeDyld::MemoryManager& MemMgr,
      RuntimeDyld::LoadedObjectInfo& LoadedObjInfo,
      std::map<StringRef, JITEvaluatedSymbol> Resolved,
      std::set<StringRef>& InternalSymbols);

   void onObjEmit(MaterializationResponsibility& R,
      object::OwningBinary<object::ObjectFile> O,
      std::unique_ptr<RuntimeDyld::MemoryManager> MemMgr,
      std::unique_ptr<RuntimeDyld::LoadedObjectInfo> LoadedObjInfo,
      Error Err);

   Error handleRemoveResources(ResourceKey K) override;
   void handleTransferResources(ResourceKey DstKey, ResourceKey SrcKey) override;

   mutable std::mutex RTDyldLayerMutex;
   DenseMap<ResourceKey, std::vector<MemoryManagerUP>> MemMgrs;
};


RTObjectLinkingLayer::RTObjectLinkingLayer(ExecutionSession& ES, GetMemoryManagerFunction GetMemoryManager)
   : ObjectLayer(ES), GetMemoryManager(GetMemoryManager)
{
   ES.registerResourceManager(*this);
}

RTObjectLinkingLayer::~RTObjectLinkingLayer() {
   assert(MemMgrs.empty() && "Layer destroyed with resources still attached");
}

void RTObjectLinkingLayer::emit(
   std::unique_ptr<MaterializationResponsibility> R,
   std::unique_ptr<MemoryBuffer> O) {
   assert(O && "Object must not be null");

   auto& ES = getExecutionSession();

   auto Obj = object::ObjectFile::createObjectFile(*O);

   if (!Obj) {
      getExecutionSession().reportError(Obj.takeError());
      R->failMaterialization();
      return;
   }

   // Collect the internal symbols from the object file: We will need to
   // filter these later.
   auto InternalSymbols = std::make_shared<std::set<StringRef>>();
   {
      for (auto& Sym : (*Obj)->symbols()) {

         // Skip file symbols.
         if (auto SymType = Sym.getType()) {
            if (*SymType == object::SymbolRef::ST_File)
               continue;
         }
         else {
            ES.reportError(SymType.takeError());
            R->failMaterialization();
            return;
         }

         Expected<uint32_t> SymFlagsOrErr = Sym.getFlags();
         if (!SymFlagsOrErr) {
            // TODO: Test this error.
            ES.reportError(SymFlagsOrErr.takeError());
            R->failMaterialization();
            return;
         }

         // Don't include symbols that aren't global.
         if (!(*SymFlagsOrErr & object::BasicSymbolRef::SF_Global)) {
            if (auto SymName = Sym.getName())
               InternalSymbols->insert(*SymName);
            else {
               ES.reportError(SymName.takeError());
               R->failMaterialization();
               return;
            }
         }
      }
   }

   auto MemMgr = GetMemoryManager();
   auto& MemMgrRef = *MemMgr;

   // Switch to shared ownership of MR so that it can be captured by both
   // lambdas below.
   std::shared_ptr<MaterializationResponsibility> SharedR(std::move(R));

   class JITDylibSearchOrderResolver : public JITSymbolResolver {
   public:
      JITDylibSearchOrderResolver(MaterializationResponsibility& MR) : MR(MR) {}

      void lookup(const LookupSet& Symbols, OnResolvedFunction OnResolved) override {
         auto& ES = MR.getTargetJITDylib().getExecutionSession();
         SymbolLookupSet InternedSymbols;

         // Intern the requested symbols: lookup takes interned strings.
         for (auto& S : Symbols)
            InternedSymbols.add(ES.intern(S));

         // Build an OnResolve callback to unwrap the interned strings and pass them
         // to the OnResolved callback.
         auto OnResolvedWithUnwrap =
            [OnResolved = std::move(OnResolved)](
               Expected<SymbolMap> InternedResult) mutable {
            if (!InternedResult) {
               OnResolved(InternedResult.takeError());
               return;
            }

            LookupResult Result;
            for (auto& KV : *InternedResult)
               Result[*KV.first] = std::move(KV.second);
            OnResolved(Result);
         };

         // Register dependencies for all symbols contained in this set.
         auto RegisterDependencies = [&](const SymbolDependenceMap& Deps) {
            MR.addDependenciesForAll(Deps);
         };

         JITDylibSearchOrder LinkOrder;
         MR.getTargetJITDylib().withLinkOrderDo(
            [&](const JITDylibSearchOrder& LO) { LinkOrder = LO; });
         ES.lookup(LookupKind::Static, LinkOrder, InternedSymbols,
            SymbolState::Resolved, std::move(OnResolvedWithUnwrap),
            RegisterDependencies);
      }

      Expected<LookupSet> getResponsibilitySet(const LookupSet& Symbols) override {
         LookupSet Result;

         for (auto& KV : MR.getSymbols()) {
            if (Symbols.count(*KV.first))
               Result.insert(*KV.first);
         }

         return Result;
      }

   private:
      MaterializationResponsibility& MR;
   } Resolver(*SharedR);

   jitLinkForORC(
      object::OwningBinary<object::ObjectFile>(std::move(*Obj), std::move(O)),
      MemMgrRef, Resolver, ProcessAllSections,
      [this, SharedR, &MemMgrRef, InternalSymbols](
         const object::ObjectFile& Obj, RuntimeDyld::LoadedObjectInfo& LoadedObjInfo,
         std::map<StringRef, JITEvaluatedSymbol> ResolvedSymbols)
      {
         return onObjLoad(*SharedR, Obj, MemMgrRef, LoadedObjInfo, ResolvedSymbols, *InternalSymbols);
      },
      [this, SharedR, MemMgr = std::move(MemMgr)](
         object::OwningBinary<object::ObjectFile> Obj,
         std::unique_ptr<RuntimeDyld::LoadedObjectInfo> LoadedObjInfo, Error Err) mutable
      {
         onObjEmit(*SharedR, std::move(Obj), std::move(MemMgr), std::move(LoadedObjInfo), std::move(Err));
      });
}

Error RTObjectLinkingLayer::onObjLoad(
   MaterializationResponsibility& R, const object::ObjectFile& Obj,
   RuntimeDyld::MemoryManager& MemMgr,
   RuntimeDyld::LoadedObjectInfo& LoadedObjInfo,
   std::map<StringRef, JITEvaluatedSymbol> Resolved,
   std::set<StringRef>& InternalSymbols) {
   SymbolFlagsMap ExtraSymbolsToClaim;
   SymbolMap Symbols;

   /// Instructs this RTDyldLinkingLayer2 instance to override the symbol flags
   /// returned by RuntimeDyld for any given object file with the flags supplied
   /// by the MaterializationResponsibility instance. This is a workaround to
   /// support symbol visibility in COFF, which does not use the libObject's
   /// SF_Exported flag. Use only when generating / adding COFF object files.
   ///
   /// FIXME: We should be able to remove this if/when COFF properly tracks
   /// exported symbols.
   bool OverrideObjectFlags = false;

   /// If set, this RTDyldObjectLinkingLayer instance will claim responsibility
   /// for any symbols provided by a given object file that were not already in
   /// the MaterializationResponsibility instance. Setting this flag allows
   /// higher-level program representations (e.g. LLVM IR) to be added based on
   /// only a subset of the symbols they provide, without having to write
   /// intervening layers to scan and add the additional symbols. This trades
   /// diagnostic quality for convenience however: If all symbols are enumerated
   /// up-front then clashes can be detected and reported early (and usually
   /// deterministically). If this option is set, clashes for the additional
   /// symbols may not be detected until late, and detection may depend on
   /// the flow of control through JIT'd code. Use with care.
   bool AutoClaimObjectSymbols = false;

   // Hack to support COFF constant pool comdats introduced during compilation:
   // (See http://llvm.org/PR40074)
   if (auto* COFFObj = dyn_cast<object::COFFObjectFile>(&Obj)) {
      auto& ES = getExecutionSession();
      OverrideObjectFlags = true;
      AutoClaimObjectSymbols = true;

      // For all resolved symbols that are not already in the responsibilty set:
      // check whether the symbol is in a comdat section and if so mark it as
      // weak.
      for (auto& Sym : COFFObj->symbols()) {
         // getFlags() on COFF symbols can't fail.
         uint32_t SymFlags = cantFail(Sym.getFlags());
         if (SymFlags & object::BasicSymbolRef::SF_Undefined)
            continue;
         auto Name = Sym.getName();
         if (!Name)
            return Name.takeError();
         auto I = Resolved.find(*Name);

         // Skip unresolved symbols, internal symbols, and symbols that are
         // already in the responsibility set.
         if (I == Resolved.end() || InternalSymbols.count(*Name) ||
            R.getSymbols().count(ES.intern(*Name)))
            continue;
         auto Sec = Sym.getSection();
         if (!Sec)
            return Sec.takeError();
         if (*Sec == COFFObj->section_end())
            continue;
         auto& COFFSec = *COFFObj->getCOFFSection(**Sec);
         if (COFFSec.Characteristics & COFF::IMAGE_SCN_LNK_COMDAT)
            I->second.setFlags(I->second.getFlags() | JITSymbolFlags::Weak);
      }
   }

   for (auto& KV : Resolved) {
      // Scan the symbols and add them to the Symbols map for resolution.

      // We never claim internal symbols.
      if (InternalSymbols.count(KV.first))
         continue;

      auto InternedName = getExecutionSession().intern(KV.first);
      auto Flags = KV.second.getFlags();

      // Override object flags and claim responsibility for symbols if
      // requested.
      if (OverrideObjectFlags || AutoClaimObjectSymbols) {
         auto I = R.getSymbols().find(InternedName);

         if (OverrideObjectFlags && I != R.getSymbols().end())
            Flags = I->second;
         else if (AutoClaimObjectSymbols && I == R.getSymbols().end())
            ExtraSymbolsToClaim[InternedName] = Flags;
      }

      Symbols[InternedName] = JITEvaluatedSymbol(KV.second.getAddress(), Flags);
   }

   if (!ExtraSymbolsToClaim.empty()) {
      if (auto Err = R.defineMaterializing(ExtraSymbolsToClaim))
         return Err;

      // If we claimed responsibility for any weak symbols but were rejected then
      // we need to remove them from the resolved set.
      for (auto& KV : ExtraSymbolsToClaim)
         if (KV.second.isWeak() && !R.getSymbols().count(KV.first))
            Symbols.erase(KV.first);
   }

   if (auto Err = R.notifyResolved(Symbols)) {
      R.failMaterialization();
      return Err;
   }

   /*if (NotifyLoaded)
      NotifyLoaded(R, Obj, LoadedObjInfo);*/

   return Error::success();
}

void RTObjectLinkingLayer::onObjEmit(
   MaterializationResponsibility& R,
   object::OwningBinary<object::ObjectFile> O,
   std::unique_ptr<RuntimeDyld::MemoryManager> MemMgr,
   std::unique_ptr<RuntimeDyld::LoadedObjectInfo> LoadedObjInfo, Error Err) {
   if (Err) {
      getExecutionSession().reportError(std::move(Err));
      R.failMaterialization();
      return;
   }

   if (auto Err = R.notifyEmitted()) {
      getExecutionSession().reportError(std::move(Err));
      R.failMaterialization();
      return;
   }

   std::unique_ptr<object::ObjectFile> Obj;
   std::unique_ptr<MemoryBuffer> ObjBuffer;
   std::tie(Obj, ObjBuffer) = O.takeBinary();

   // Run EventListener notifyLoaded callbacks.
   {
      std::lock_guard<std::mutex> Lock(RTDyldLayerMutex);
      for (auto* L : EventListeners)
         L->notifyObjectLoaded(pointerToJITTargetAddress(MemMgr.get()), *Obj,
            *LoadedObjInfo);
   }

   if (auto Err = R.withResourceKeyDo(
      [&](ResourceKey K) { MemMgrs[K].push_back(std::move(MemMgr)); })) {
      getExecutionSession().reportError(std::move(Err));
      R.failMaterialization();
   }
}

Error RTObjectLinkingLayer::handleRemoveResources(ResourceKey K) {

   std::vector<MemoryManagerUP> MemMgrsToRemove;

   getExecutionSession().runSessionLocked([&] {
      auto I = MemMgrs.find(K);
      if (I != MemMgrs.end()) {
         std::swap(MemMgrsToRemove, I->second);
         MemMgrs.erase(I);
      }
      });

   {
      std::lock_guard<std::mutex> Lock(RTDyldLayerMutex);
      for (auto& MemMgr : MemMgrsToRemove) {
         for (auto* L : EventListeners)
            L->notifyFreeingObject(pointerToJITTargetAddress(MemMgr.get()));
         MemMgr->deregisterEHFrames();
      }
   }

   return Error::success();
}

void RTObjectLinkingLayer::handleTransferResources(ResourceKey DstKey,
   ResourceKey SrcKey) {
   auto I = MemMgrs.find(SrcKey);
   if (I != MemMgrs.end()) {
      auto& SrcMemMgrs = I->second;
      auto& DstMemMgrs = MemMgrs[DstKey];
      DstMemMgrs.reserve(DstMemMgrs.size() + SrcMemMgrs.size());
      for (auto& MemMgr : SrcMemMgrs)
         DstMemMgrs.push_back(std::move(MemMgr));

      // Erase SrcKey entry using value rather than iterator I: I may have been
      // invalidated when we looked up DstKey.
      MemMgrs.erase(SrcKey);
   }
}

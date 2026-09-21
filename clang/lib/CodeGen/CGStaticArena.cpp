//===--- CGStaticArena.cpp - Per-invocation static storage ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/DiagnosticCodeGen.h"
#include "clang/Basic/StaticArenaList.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/StaticArenaAddressEscape.h"

using namespace clang;
using namespace clang::CodeGen;

namespace {

constexpr llvm::StringLiteral StaticArenaAttribute = "static-arena";
constexpr llvm::StringLiteral StaticArenaRecordPrefix = "__llvm_arena_var_v1.";
constexpr llvm::StringLiteral StaticArenaTemplatePrefix =
    "__llvm_arena_template_v1.";
constexpr llvm::StringLiteral StaticArenaEntryPrefix = "__llvm_arena_ptr_v1.";
constexpr llvm::StringLiteral StaticArenaNamePrefix = "__llvm_arena_name_v1.";

static void copyStaticArenaProperties(const llvm::GlobalVariable &From,
                                      llvm::GlobalVariable &To) {
  To.setVisibility(From.getVisibility());
  To.setDSOLocal(From.isDSOLocal());
  To.setPartition(From.getPartition());
}

static const llvm::GlobalVariable *
findReferencedArenaGlobal(const llvm::Constant *Root) {
  llvm::SmallVector<const llvm::Constant *, 16> Worklist(1, Root);
  llvm::SmallPtrSet<const llvm::Constant *, 16> Visited;
  while (!Worklist.empty()) {
    const llvm::Constant *C = Worklist.pop_back_val();
    if (!Visited.insert(C).second)
      continue;
    if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(C)) {
      if (CodeGenModule::isStaticArenaGlobal(GV))
        return GV;
      continue;
    }
    for (const llvm::Use &U : C->operands())
      if (const auto *Child = llvm::dyn_cast<llvm::Constant>(U.get()))
        Worklist.push_back(Child);
  }
  return nullptr;
}

static bool hasDynamicInitialization(const VarDecl *D) {
  if (!D)
    return false;

  const VarDecl *InitDecl = D;
  const Expr *Init = D->getAnyInitializer(InitDecl);
  return Init && !InitDecl->hasConstantInitialization();
}

/// Return whether an arena-selected C++ variable must be constructed for each
/// arena instead of being initialized by copying its process-lifetime constant
/// representation.  Copying is only sound for types which promise that every
/// bitwise clone is an independent object.  In particular, a constexpr default
/// constructor does not make a std::unique_ptr-like object safe to clone.
static bool requiresRuntimeInitialization(const CodeGenModule &CGM,
                                          const VarDecl *D) {
  if (!D || !CGM.getLangOpts().CPlusPlus)
    return false;

  const VarDecl *InitDecl = D;
  const Expr *Init = D->getAnyInitializer(InitDecl);
  if (!Init || !InitDecl->hasConstantInitialization())
    return false;

  QualType Type = InitDecl->getType();
  return !Type.isTriviallyCopyableType(CGM.getContext()) ||
         !Type.isBitwiseCloneableType(CGM.getContext());
}

} // namespace

bool CodeGenModule::isStaticArenaVar(const VarDecl *D, StringRef MangledName) {
  if (getCodeGenOpts().StaticArenaLifecycleId.empty() || !D)
    return false;

  if (!StaticArenaSelection || StaticArenaSelection->isEmpty())
    return false;

  if (MangledName.empty()) {
    if (D->isFileVarDecl() || D->isStaticDataMember())
      MangledName = getMangledName(GlobalDecl(D));
    else
      MangledName = D->getName();
  }

  const VarDecl *MatchKey = D;
  auto &MatchesByDecl = StaticArenaVarMatchCache[MangledName];
  auto MatchIt = MatchesByDecl.find(MatchKey);
  bool Matches =
      MatchIt != MatchesByDecl.end()
          ? MatchIt->second
          : MatchesByDecl
                .try_emplace(MatchKey,
                             matchesList(*StaticArenaSelection,
                                         StaticArenaTypeCache, D, MangledName))
                .first->second;
  if (!Matches)
    return false;

  // The remaining checks are properties of the declaration, not of the name
  // by which this particular caller reached it. Memoizing them separately also
  // keeps rejection diagnostics to one per declaration.
  const VarDecl *EligibilityKey = D;
  if (auto It = StaticArenaVarCache.find(EligibilityKey);
      It != StaticArenaVarCache.end())
    return It->second;

  auto Reject = [&](unsigned DiagID, StringRef Detail = {}) {
    auto DB = getDiags().Report(D->getLocation(), DiagID);
    DB << D;
    if (!Detail.empty())
      DB << Detail;
    StaticArenaVarCache[EligibilityKey] = false;
    return false;
  };

  if (D->isStaticDataMember() &&
      (D->isTemplated() ||
       isa<ClassTemplateSpecializationDecl>(D->getDeclContext())))
    return Reject(diag::err_static_arena_template_static);
  if (D->hasAttr<OMPThreadPrivateDeclAttr>())
    return Reject(diag::err_static_arena_threadprivate);
  if (D->getType().getAddressSpace() != LangAS::Default)
    return Reject(diag::err_static_arena_address_space);
  if (D->getType()->isReferenceType())
    return Reject(diag::err_static_arena_reference);
  if (!D->getType()->isIncompleteType() &&
      getContext().getTypeSizeInChars(D->getType()).isZero())
    return Reject(diag::err_static_arena_zero_size);
  if (D->isConstexpr() || D->isUsableInConstantExpressions(getContext()))
    return Reject(diag::err_static_arena_constexpr);
  if (D->hasAttr<DLLImportAttr>() || D->hasAttr<DLLExportAttr>())
    return Reject(diag::err_static_arena_dll_storage);
  if (D->hasAttr<AliasAttr>() || D->hasAttr<WeakRefAttr>())
    return Reject(diag::err_static_arena_alias);
  if (D->hasAttr<UsedAttr>() || D->hasAttr<RetainAttr>())
    return Reject(diag::err_static_arena_retained);
  if (D->hasAttr<InitPriorityAttr>() || D->hasAttr<InitSegAttr>())
    return Reject(diag::err_static_arena_explicit_init_order);
  if (D->getTLSKind())
    return Reject(diag::err_static_arena_conflicting_storage, "thread-local");
  if (D->hasAttr<SectionAttr>() || D->hasAttr<PragmaClangBSSSectionAttr>() ||
      D->hasAttr<PragmaClangDataSectionAttr>() ||
      D->hasAttr<PragmaClangRodataSectionAttr>() ||
      D->hasAttr<PragmaClangRelroSectionAttr>())
    return Reject(diag::err_static_arena_conflicting_storage, "section");
  if (D->hasAttr<LoaderUninitializedAttr>())
    return Reject(diag::err_static_arena_conflicting_storage,
                  "loader-uninitialized");
  if (D->getStorageClass() == SC_Register && D->hasAttr<AsmLabelAttr>())
    return Reject(diag::err_static_arena_conflicting_storage, "named-register");
  if (D->hasAttr<CommonAttr>() ||
      (!getCodeGenOpts().NoCommon && !D->hasAttr<NoCommonAttr>() &&
       D->isThisDeclarationADefinition(getContext()) ==
           VarDecl::TentativeDefinition))
    return Reject(diag::err_static_arena_common);
  if ((D->hasAttr<WeakAttr>() || D->hasAttr<WeakImportAttr>() ||
       D->hasAttr<WeakRefAttr>()) &&
      D->hasDefinition(getContext()) == VarDecl::DeclarationOnly)
    return Reject(diag::err_static_arena_undefined_weak);

  VarDecl::DefinitionKind DefinitionKind = D->hasDefinition(getContext());
  bool HasDefinition = DefinitionKind != VarDecl::DeclarationOnly;
  const VarDecl *Definition = D->getDefinition(getContext());
  if (!Definition && DefinitionKind == VarDecl::TentativeDefinition)
    Definition = D->getActingDefinition();
  if (!Definition)
    Definition = D;
  bool HasDynamicInitialization = hasDynamicInitialization(Definition);
  bool RequiresRuntimeInitialization =
      requiresRuntimeInitialization(*this, Definition);
  QualType DefinitionType = Definition->getType();
  QualType::DestructionKind Destruction =
      Definition->needsDestruction(getContext());
  if (HasDefinition && Destruction != QualType::DK_none &&
      ((!HasDynamicInitialization && !RequiresRuntimeInitialization) ||
       Destruction != QualType::DK_cxx_destructor ||
       !DefinitionType->getAsCXXRecordDecl()))
    return Reject(diag::err_static_arena_unsupported_destructor);
  if (HasDefinition && !DefinitionType->isIncompleteType() &&
      !HasDynamicInitialization && !RequiresRuntimeInitialization &&
      (!DefinitionType.isTriviallyCopyableType(getContext()) ||
       !DefinitionType.isBitwiseCloneableType(getContext())))
    return Reject(diag::err_static_arena_constant_object);
  if (D->isStaticLocal() && getTarget().getCXXABI().isMicrosoft() &&
      !getLangOpts().ThreadsafeStatics)
    return Reject(diag::err_static_arena_requires_threadsafe_statics);

  StaticArenaVarCache[EligibilityKey] = true;
  return true;
}

bool CodeGenModule::isStaticArenaInitializer(const VarDecl *D) {
  // This helper is reached from ordinary global-initializer emission as well
  // as from the arena-specific paths.  In particular, CUDA/HIP may emit a
  // device-side local-static initializer without materializing the
  // corresponding host storage in this module.  Do not require arena storage
  // invariants unless arena lowering is actually enabled for the TU.
  if (getCodeGenOpts().StaticArenaLifecycleId.empty() || !D)
    return false;

  // getOrCreateStaticVarDecl creates and classifies local-static storage
  // before emitting its initializer. In C, that storage is named for its
  // enclosing context (for example, "function.p"), while getMangledName(D)
  // only produces the source name ("p"). Preserve the materialized decision
  // instead of re-matching the declaration under a different name.
  if (D->isStaticLocal()) {
    llvm::Constant *Addr = getStaticLocalDeclAddress(D);
    assert(Addr && "local-static initializer emitted before its storage");
    return isStaticArenaGlobal(
        llvm::dyn_cast<llvm::GlobalVariable>(Addr->stripPointerCasts()));
  }

  return isStaticArenaVar(D, getMangledName(GlobalDecl(D)));
}

bool CodeGenModule::requiresStaticArenaRuntimeInitialization(const VarDecl *D) {
  return isStaticArenaInitializer(D) && requiresRuntimeInitialization(*this, D);
}

llvm::GlobalVariable *
CodeGenModule::getOrCreateStaticArenaRecord(const VarDecl *D,
                                            StringRef StorageName) {
  StaticArenaGlobalInfo &Info = StaticArenaGlobals[StorageName];
  if (D && !Info.D)
    Info.D = D->getCanonicalDecl();
  if (Info.Record)
    return Info.Record;

  llvm::PointerType *PtrTy = llvm::PointerType::getUnqual(getLLVMContext());
  llvm::StructType *RecordTy = llvm::StructType::get(
      getLLVMContext(), {Int64Ty, Int64Ty, Int64Ty, PtrTy, PtrTy});
  std::string RecordName = (StaticArenaRecordPrefix + StorageName).str();
  llvm::GlobalVariable *Record = getModule().getNamedGlobal(RecordName);
  if (!Record)
    Record =
        new llvm::GlobalVariable(getModule(), RecordTy, /*isConstant=*/false,
                                 llvm::GlobalValue::ExternalLinkage,
                                 /*Initializer=*/nullptr, RecordName);
  Record->setExternallyInitialized(true);
  Record->addAttribute(StaticArenaAttribute);
  Info.Record = Record;
  return Record;
}

void CodeGenModule::markStaticArenaPlaceholder(const VarDecl *D,
                                               llvm::GlobalVariable *Storage) {
  if (!isStaticArenaVar(D, Storage->getName()))
    return;
  Storage->addAttribute(StaticArenaAttribute);
  llvm::GlobalVariable *Record =
      getOrCreateStaticArenaRecord(D, Storage->getName());
  // An available_externally initializer can be attached to a declaration-only
  // static data member as an optimization aid.  The corresponding arena record
  // remains an initializer-less declaration, for which available_externally is
  // not a valid linkage.
  if (!Record->hasInitializer() && Storage->hasAvailableExternallyLinkage())
    Record->setLinkage(llvm::GlobalValue::ExternalLinkage);
  else
    Record->setLinkage(Storage->getLinkage());
  copyStaticArenaProperties(*Storage, *Record);
  // Local-static guarded initialization can be emitted before the owner's
  // finalization. Establish the record-keyed group as soon as the placeholder
  // already has a COMDAT so the subsequently created guard, lifecycle entry,
  // and owner cannot be split across independently coalesced groups.
  if (llvm::Comdat *StorageComdat = Storage->getComdat()) {
    llvm::Comdat *RecordComdat =
        getModule().getOrInsertComdat(Record->getName());
    RecordComdat->setSelectionKind(StorageComdat->getSelectionKind());
    Storage->setComdat(RecordComdat);
    Record->setComdat(RecordComdat);
  }
}

llvm::GlobalVariable *
CodeGenModule::getStaticArenaRecord(const VarDecl *D,
                                    llvm::GlobalVariable *Storage) {
  markStaticArenaPlaceholder(D, Storage);
  if (!isStaticArenaGlobal(Storage))
    return nullptr;
  return getOrCreateStaticArenaRecord(D, Storage->getName());
}

llvm::Comdat *
CodeGenModule::getStaticArenaComdat(llvm::GlobalVariable *Storage) {
  auto It = StaticArenaGlobals.find(Storage->getName());
  if (It == StaticArenaGlobals.end() || !It->second.Record)
    return nullptr;
  return It->second.Record->getComdat();
}

void CodeGenModule::finalizeStaticArenaGlobal(const VarDecl *D,
                                              llvm::GlobalVariable *Storage,
                                              llvm::Comdat *OwnerComdat) {
  if (!isStaticArenaGlobal(Storage))
    return;

  StaticArenaGlobalInfo &Info = StaticArenaGlobals[Storage->getName()];
  if (Info.IsFinalized)
    return;

  llvm::GlobalVariable *Record =
      getOrCreateStaticArenaRecord(D, Storage->getName());
  Record->setLinkage(Storage->getLinkage());
  copyStaticArenaProperties(*Storage, *Record);

  llvm::Comdat *C = OwnerComdat;
  if (!C && Storage->isWeakForLinker()) {
    if (!supportsCOMDAT()) {
      if (D)
        getDiags().Report(D->getLocation(),
                          diag::err_static_arena_conflicting_storage)
            << D << "coalescable linkage without COMDAT support";
      return;
    }
    llvm::Comdat *OriginalComdat = Storage->getComdat();
    C = getModule().getOrInsertComdat(Record->getName());
    if (OriginalComdat)
      C->setSelectionKind(OriginalComdat->getSelectionKind());
  }
  if (C) {
    Storage->setComdat(C);
    Record->setComdat(C);
  }

  if (!Storage->getValueType()->isSized()) {
    if (D)
      getDiags().Report(D->getLocation(), diag::err_static_arena_zero_size)
          << D;
    return;
  }
  llvm::TypeSize AllocSize =
      getDataLayout().getTypeAllocSize(Storage->getValueType());
  if (AllocSize.isScalable() || AllocSize.getFixedValue() == 0) {
    if (D)
      getDiags().Report(D->getLocation(), diag::err_static_arena_zero_size)
          << D;
    return;
  }
  llvm::Align Alignment = Storage->getAlign().value_or(
      getDataLayout().getABITypeAlign(Storage->getValueType()));

  llvm::PointerType *PtrTy = llvm::PointerType::getUnqual(getLLVMContext());
  llvm::Constant *NullPtr = llvm::ConstantPointerNull::get(PtrTy);
  llvm::Constant *TemplatePtr = NullPtr;
  llvm::Constant *Init = Storage->getInitializer();
  bool HasRuntimeInitialization =
      hasDynamicInitialization(D) || requiresRuntimeInitialization(*this, D);
  if (Init && !Init->isNullValue() && !HasRuntimeInitialization) {
    if (const llvm::GlobalVariable *Ref = findReferencedArenaGlobal(Init)) {
      getDiags().Report(D ? D->getLocation() : SourceLocation(),
                        diag::err_static_arena_template_reference)
          << (D ? D->getName() : Storage->getName()) << Ref->getName();
      // Keep the already-invalid initializer from retaining a selected
      // placeholder and producing a second, less useful direct-use error at
      // module finalization.
      Storage->setInitializer(
          llvm::Constant::getNullValue(Storage->getValueType()));
    } else {
      std::string TemplateName =
          (StaticArenaTemplatePrefix + Storage->getName()).str();
      Info.Template = new llvm::GlobalVariable(
          getModule(), Storage->getValueType(), /*isConstant=*/true,
          Storage->getLinkage(), Init, TemplateName);
      Info.Template->setAlignment(Alignment);
      copyStaticArenaProperties(*Storage, *Info.Template);
      if (C)
        Info.Template->setComdat(C);
      TemplatePtr = Info.Template;
    }
  }

  std::string NameGlobalName =
      (StaticArenaNamePrefix + Storage->getName()).str();
  llvm::Constant *NameInit =
      llvm::ConstantDataArray::getString(getLLVMContext(), Storage->getName());
  auto *NameGlobal = new llvm::GlobalVariable(
      getModule(), NameInit->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, NameInit, NameGlobalName);
  NameGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  NameGlobal->setAlignment(llvm::Align(1));

  llvm::Constant *Fields[] = {
      llvm::ConstantInt::get(Int64Ty, UINT64_MAX),
      llvm::ConstantInt::get(Int64Ty, AllocSize.getFixedValue()),
      llvm::ConstantInt::get(Int64Ty, Alignment.value()), TemplatePtr,
      NameGlobal};
  Record->setInitializer(llvm::ConstantStruct::get(
      cast<llvm::StructType>(Record->getValueType()), Fields));
  Record->setAlignment(getDataLayout().getABITypeAlign(Record->getValueType()));

  std::string EntryName = (StaticArenaEntryPrefix + Storage->getName()).str();
  Info.Entry = new llvm::GlobalVariable(getModule(), PtrTy, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        Record, EntryName);
  Info.Entry->setSection(getTriple().isOSBinFormatCOFF() ? ".llvma$v1$b"
                                                         : "llvma_v1");
  Info.Entry->setAlignment(getDataLayout().getPointerABIAlignment(0));
  if (C)
    Info.Entry->setComdat(C);
  addUsedGlobal(Info.Entry);
  Info.IsFinalized = true;
}

Address CodeGenModule::emitStaticArenaAddress(CodeGenFunction &CGF,
                                              const VarDecl *D,
                                              llvm::GlobalVariable *Storage) {
  llvm::GlobalVariable *Record = getStaticArenaRecord(D, Storage);
  assert(Record && "address requested for a non-arena variable");
  llvm::PointerType *PtrTy = llvm::PointerType::getUnqual(getLLVMContext());
  llvm::FunctionType *ResolverTy =
      llvm::FunctionType::get(PtrTy, {PtrTy}, /*isVarArg=*/false);
  llvm::FunctionCallee Resolver =
      CreateRuntimeFunction(ResolverTy, "__llvm_arena_addr_v1");
  llvm::Value *Pointer =
      CGF.Builder.CreateCall(Resolver, {Record}, Storage->getName() + ".arena");
  // A declaration-only record can intentionally have an opaque LLVM type.
  // Do not eagerly compute its ABI alignment when Clang already set one.
  llvm::MaybeAlign ExplicitAlignment = Storage->getAlign();
  llvm::Align Alignment =
      ExplicitAlignment
          ? *ExplicitAlignment
          : getDataLayout().getABITypeAlign(Storage->getValueType());
  return Address(Pointer, Storage->getValueType(),
                 CharUnits::fromQuantity(Alignment.value()));
}

Address CodeGenModule::emitStaticArenaAddress(CodeGenFunction &CGF,
                                              llvm::GlobalVariable *Storage) {
  auto It = StaticArenaGlobals.find(Storage->getName());
  assert(It != StaticArenaGlobals.end() && It->second.Record &&
         "unregistered static-arena storage");
  llvm::PointerType *PtrTy = llvm::PointerType::getUnqual(getLLVMContext());
  llvm::FunctionType *ResolverTy =
      llvm::FunctionType::get(PtrTy, {PtrTy}, /*isVarArg=*/false);
  llvm::FunctionCallee Resolver =
      CreateRuntimeFunction(ResolverTy, "__llvm_arena_addr_v1");
  llvm::Value *Pointer = CGF.Builder.CreateCall(Resolver, {It->second.Record},
                                                Storage->getName() + ".arena");
  llvm::Align Alignment = Storage->getAlign().value_or(
      getDataLayout().getABITypeAlign(Storage->getValueType()));
  return Address(Pointer, Storage->getValueType(),
                 CharUnits::fromQuantity(Alignment.value()));
}

void CodeGenModule::registerStaticArenaGuard(
    const VarDecl &Owner, llvm::GlobalVariable *Guard,
    llvm::GlobalVariable *OwnerStorage) {
  Guard->addAttribute(StaticArenaAttribute);
  StaticArenaGlobalInfo &Info = StaticArenaGlobals[Guard->getName()];
  Info.D = Owner.getCanonicalDecl();
  getOrCreateStaticArenaRecord(&Owner, Guard->getName());
  finalizeStaticArenaGlobal(&Owner, Guard, getStaticArenaComdat(OwnerStorage));
}

void CodeGenModule::checkStaticArenaPlaceholders() {
  if (getCodeGenOpts().StaticArenaLifecycleId.empty())
    return;

  // Catch the known cached-address shape before optimization and before the
  // selected storage placeholders are erased.
  for (const llvm::StaticArenaAddressEscape &Escape :
       llvm::findStaticArenaAddressEscapes(getModule())) {
    auto It = StaticArenaGlobals.find(Escape.getArenaStorageName());
    SourceLocation Loc = It != StaticArenaGlobals.end() && It->second.D
                             ? It->second.D->getLocation()
                             : SourceLocation();
    getDiags().Report(Loc, diag::err_static_arena_address_escape)
        << Escape.getArenaStorageName() << Escape.getDestinationName();
  }

  for (auto &Item : StaticArenaGlobals) {
    llvm::GlobalVariable *Storage = getModule().getNamedGlobal(Item.getKey());
    if (!Storage || !isStaticArenaGlobal(Storage))
      continue;
    if (!Storage->use_empty()) {
      SourceLocation Loc = Item.getValue().D ? Item.getValue().D->getLocation()
                                             : SourceLocation();
      getDiags().Report(Loc, diag::err_static_arena_direct_use)
          << Storage->getName();
      continue;
    }
    Storage->eraseFromParent();
  }
}

//===--- X86DomainReassignment.cpp - Selectively switch register classes---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass attempts to find instruction chains (closures) in one domain,
// and convert them to equivalent instructions in a different domain,
// if profitable.
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Printable.h"
#include <bitset>

using namespace llvm;

#define DEBUG_TYPE "x86-domain-reassignment"

STATISTIC(NumClosuresConverted, "Number of closures converted by the pass");

static cl::opt<bool> DisableX86DomainReassignment(
    "disable-x86-domain-reassignment", cl::Hidden,
    cl::desc("X86: Disable Virtual Register Reassignment."), cl::init(false));

namespace {
enum RegDomain { NoDomain = -1, GPRDomain, MaskDomain, OtherDomain, NumDomains };

static bool isMask(const TargetRegisterClass *RC,
                   const TargetRegisterInfo *TRI) {
  return X86::VK16RegClass.hasSubClassEq(RC);
}

static RegDomain getDomain(const TargetRegisterClass *RC,
                           const TargetRegisterInfo *TRI) {
  if (TRI->isGeneralPurposeRegisterClass(RC))
    return GPRDomain;
  if (isMask(RC, TRI))
    return MaskDomain;
  return OtherDomain;
}

/// Return a register class equivalent to \p SrcRC, in \p Domain.
static const TargetRegisterClass *getDstRC(const TargetRegisterClass *SrcRC,
                                           RegDomain Domain) {
  assert(Domain == MaskDomain && "add domain");
  if (X86::GR8RegClass.hasSubClassEq(SrcRC))
    return &X86::VK8RegClass;
  if (X86::GR16RegClass.hasSubClassEq(SrcRC))
    return &X86::VK16RegClass;
  if (X86::GR32RegClass.hasSubClassEq(SrcRC))
    return &X86::VK32RegClass;
  if (X86::GR64RegClass.hasSubClassEq(SrcRC))
    return &X86::VK64RegClass;
  llvm_unreachable("add register class");
  return nullptr;
}

/// Abstract Instruction Converter class.
class InstrConverterBase {
protected:
  unsigned SrcOpcode;

public:
  InstrConverterBase(unsigned SrcOpcode) : SrcOpcode(SrcOpcode) {}

  virtual ~InstrConverterBase() = default;

  /// \returns true if \p MI is legal to convert.
  virtual bool isLegal(const MachineInstr *MI,
                       const TargetInstrInfo *TII) const {
    assert(MI->getOpcode() == SrcOpcode &&
           "Wrong instruction passed to converter");
    return true;
  }

  /// Applies conversion to \p MI.
  ///
  /// \returns true if \p MI is no longer need, and can be deleted.
  virtual bool convertInstr(MachineInstr *MI, const TargetInstrInfo *TII,
                            MachineRegisterInfo *MRI) const = 0;

  /// \returns the cost increment incurred by converting \p MI.
  virtual double getExtraCost(const MachineInstr *MI,
                              MachineRegisterInfo *MRI) const = 0;
};

/// An Instruction Converter which ignores the given instruction.
/// For example, PHI instructions can be safely ignored since only the registers
/// need to change.
class InstrIgnore : public InstrConverterBase {
public:
  InstrIgnore(unsigned SrcOpcode) : InstrConverterBase(SrcOpcode) {}

  bool convertInstr(MachineInstr *MI, const TargetInstrInfo *TII,
                    MachineRegisterInfo *MRI) const override {
    assert(isLegal(MI, TII) && "Cannot convert instruction");
    return false;
  }

  double getExtraCost(const MachineInstr *MI,
                      MachineRegisterInfo *MRI) const override {
    return 0;
  }
};

/// An Instruction Converter which replaces an instruction with another.
class InstrReplacer : public InstrConverterBase {
public:
  /// Opcode of the destination instruction.
  unsigned DstOpcode;

  InstrReplacer(unsigned SrcOpcode, unsigned DstOpcode)
      : InstrConverterBase(SrcOpcode), DstOpcode(DstOpcode) {}

  bool isLegal(const MachineInstr *MI,
               const TargetInstrInfo *TII) const override {
    if (!InstrConverterBase::isLegal(MI, TII))
      return false;
    // It's illegal to replace an instruction that implicitly defines a register
    // with an instruction that doesn't, unless that register dead.
    for (const auto &MO : MI->implicit_operands())
      if (MO.isReg() && MO.isDef() && !MO.isDead() &&
          !TII->get(DstOpcode).hasImplicitDefOfPhysReg(MO.getReg()))
        return false;
    return true;
  }

  bool convertInstr(MachineInstr *MI, const TargetInstrInfo *TII,
                    MachineRegisterInfo *MRI) const override {
    assert(isLegal(MI, TII) && "Cannot convert instruction");
    MachineInstrBuilder Bld =
        BuildMI(*MI->getParent(), MI, MI->getDebugLoc(), TII->get(DstOpcode));
    // Transfer explicit operands from original instruction. Implicit operands
    // are handled by BuildMI.
    for (auto &Op : MI->explicit_operands())
      Bld.add(Op);
    return true;
  }

  double getExtraCost(const MachineInstr *MI,
                      MachineRegisterInfo *MRI) const override {
    // Assuming instructions have the same cost.
    return 0;
  }
};

/// An Instruction Converter which replaces an instruction with another, and
/// adds a COPY from the new instruction's destination to the old one's.
class InstrReplacerDstCOPY : public InstrConverterBase {
public:
  unsigned DstOpcode;

  InstrReplacerDstCOPY(unsigned SrcOpcode, unsigned DstOpcode)
      : InstrConverterBase(SrcOpcode), DstOpcode(DstOpcode) {}

  bool convertInstr(MachineInstr *MI, const TargetInstrInfo *TII,
                    MachineRegisterInfo *MRI) const override {
    assert(isLegal(MI, TII) && "Cannot convert instruction");
    MachineBasicBlock *MBB = MI->getParent();
    const DebugLoc &DL = MI->getDebugLoc();

    Register Reg = MRI->createVirtualRegister(
        TII->getRegClass(TII->get(DstOpcode), 0, MRI->getTargetRegisterInfo(),
                         *MBB->getParent()));
    MachineInstrBuilder Bld = BuildMI(*MBB, MI, DL, TII->get(DstOpcode), Reg);
    for (const MachineOperand &MO : llvm::drop_begin(MI->operands()))
      Bld.add(MO);

    BuildMI(*MBB, MI, DL, TII->get(TargetOpcode::COPY))
        .add(MI->getOperand(0))
        .addReg(Reg);

    return true;
  }

  double getExtraCost(const MachineInstr *MI,
                      MachineRegisterInfo *MRI) const override {
    // Assuming instructions have the same cost, and that COPY is in the same
    // domain so it will be eliminated.
    return 0;
  }
};

/// An Instruction Converter for replacing COPY instructions.
class InstrCOPYReplacer : public InstrReplacer {
public:
  RegDomain DstDomain;

  InstrCOPYReplacer(unsigned SrcOpcode, RegDomain DstDomain, unsigned DstOpcode)
      : InstrReplacer(SrcOpcode, DstOpcode), DstDomain(DstDomain) {}

  bool isLegal(const MachineInstr *MI,
               const TargetInstrInfo *TII) const override {
    if (!InstrConverterBase::isLegal(MI, TII))
      return false;

    // Don't allow copies to/flow GR8/GR16 physical registers.
    // FIXME: Is there some better way to support this?
    Register DstReg = MI->getOperand(0).getReg();
    if (DstReg.isPhysical() && (X86::GR8RegClass.contains(DstReg) ||
                                X86::GR16RegClass.contains(DstReg)))
      return false;
    Register SrcReg = MI->getOperand(1).getReg();
    if (SrcReg.isPhysical() && (X86::GR8RegClass.contains(SrcReg) ||
                                X86::GR16RegClass.contains(SrcReg)))
      return false;

    return true;
  }

  double getExtraCost(const MachineInstr *MI,
                      MachineRegisterInfo *MRI) const override {
    assert(MI->getOpcode() == TargetOpcode::COPY && "Expected a COPY");

    for (const auto &MO : MI->operands()) {
      // Physical registers will not be converted. Assume that converting the
      // COPY to the destination domain will eventually result in a actual
      // instruction.
      if (MO.getReg().isPhysical())
        return 1;

      RegDomain OpDomain = getDomain(MRI->getRegClass(MO.getReg()),
                                     MRI->getTargetRegisterInfo());
      // Converting a cross domain COPY to a same domain COPY should eliminate
      // an insturction
      if (OpDomain == DstDomain)
        return -1;
    }
    return 0;
  }
};

/// An Instruction Converter which replaces an instruction with a COPY.
class InstrReplaceWithCopy : public InstrConverterBase {
public:
  // Source instruction operand Index, to be used as the COPY source.
  unsigned SrcOpIdx;

  InstrReplaceWithCopy(unsigned SrcOpcode, unsigned SrcOpIdx)
      : InstrConverterBase(SrcOpcode), SrcOpIdx(SrcOpIdx) {}

  bool convertInstr(MachineInstr *MI, const TargetInstrInfo *TII,
                    MachineRegisterInfo *MRI) const override {
    assert(isLegal(MI, TII) && "Cannot convert instruction");
    BuildMI(*MI->getParent(), MI, MI->getDebugLoc(),
            TII->get(TargetOpcode::COPY))
        .add({MI->getOperand(0), MI->getOperand(SrcOpIdx)});
    return true;
  }

  double getExtraCost(const MachineInstr *MI,
                      MachineRegisterInfo *MRI) const override {
    return 0;
  }
};

// Key type to be used by the Instruction Converters map.
// A converter is identified by <destination domain, source opcode>
typedef std::pair<int, unsigned> InstrConverterBaseKeyTy;

typedef DenseMap<InstrConverterBaseKeyTy, std::unique_ptr<InstrConverterBase>>
    InstrConverterBaseMap;

/// A closure is a set of virtual register representing all of the edges in
/// the closure, as well as all of the instructions connected by those edges.
///
/// A closure may encompass virtual registers in the same register bank that
/// have different widths. For example, it may contain 32-bit GPRs as well as
/// 64-bit GPRs.
///
/// A closure that computes an address (i.e. defines a virtual register that is
/// used in a memory operand) excludes the instructions that contain memory
/// operands using the address. Such an instruction will be included in a
/// different closure that manipulates the loaded or stored value.
class Closure {
private:
  /// Virtual registers in the closure.
  DenseSet<Register> Edges;

  /// Instructions in the closure.
  SmallVector<MachineInstr *, 8> Instrs;

  /// Domains which this closure can legally be reassigned to.
  std::bitset<NumDomains> LegalDstDomains;

  /// An ID to uniquely identify this closure, even when it gets
  /// moved around
  unsigned ID;

public:
  Closure(unsigned ID, std::initializer_list<RegDomain> LegalDstDomainList) : ID(ID) {
    for (RegDomain D : LegalDstDomainList)
      LegalDstDomains.set(D);
  }

  /// Mark this closure as illegal for reassignment to all domains.
  void setAllIllegal() { LegalDstDomains.reset(); }

  /// \returns true if this closure has domains which are legal to reassign to.
  bool hasLegalDstDomain() const { return LegalDstDomains.any(); }

  /// \returns true if is legal to reassign this closure to domain \p RD.
  bool isLegal(RegDomain RD) const { return LegalDstDomains[RD]; }

  /// Mark this closure as illegal for reassignment to domain \p RD.
  void setIllegal(RegDomain RD) { LegalDstDomains[RD] = false; }

  bool empty() const { return Edges.empty(); }

  bool insertEdge(Register Reg) { return Edges.insert(Reg).second; }

  using const_edge_iterator = DenseSet<Register>::const_iterator;
  iterator_range<const_edge_iterator> edges() const {
    return iterator_range<const_edge_iterator>(Edges.begin(), Edges.end());
  }

  void addInstruction(MachineInstr *I) {
    Instrs.push_back(I);
  }

  ArrayRef<MachineInstr *> instructions() const {
    return Instrs;
  }

  LLVM_DUMP_METHOD void dump(const MachineRegisterInfo *MRI) const {
    dbgs() << "Registers: ";
    bool First = true;
    for (Register Reg : Edges) {
      if (!First)
        dbgs() << ", ";
      First = false;
      dbgs() << printReg(Reg, MRI->getTargetRegisterInfo(), 0, MRI);
    }
    dbgs() << "\n" << "Instructions:";
    for (MachineInstr *MI : Instrs) {
      dbgs() << "\n  ";
      MI->print(dbgs());
    }
    dbgs() << "\n";
  }

  unsigned getID() const {
    return ID;
  }

};

class X86DomainReassignment : public MachineFunctionPass {
  const X86Subtarget *STI = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  const X86InstrInfo *TII = nullptr;

  /// All edges that are included in some closure
  DenseMap<Register, unsigned> EnclosedEdges;

  /// All instructions that are included in some closure.
  DenseMap<MachineInstr *, unsigned> EnclosedInstrs;

public:
  static char ID;

  X86DomainReassignment() : MachineFunctionPass(ID) { }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override {
    return "X86 Domain Reassignment Pass";
  }

private:
  /// A map of available Instruction Converters.
  InstrConverterBaseMap Converters;

  /// Initialize Converters map.
  void initConverters();

  /// Starting from \Reg, expand the closure as much as possible.
  void buildClosure(Closure &, Register Reg);

  /// Enqueue \p Reg to be considered for addition to the closure.
  /// Return false if the closure becomes invalid.
  bool visitRegister(Closure &, Register Reg, RegDomain &Domain,
                     SmallVectorImpl<Register> &Worklist);

  /// Reassign the closure to \p Domain.
  void reassign(const Closure &C, RegDomain Domain) const;

  /// Add \p MI to the closure.
  /// Return false if the closure becomes invalid.
  bool encloseInstr(Closure &C, MachineInstr *MI);

  /// /returns true if it is profitable to reassign the closure to \p Domain.
  bool isReassignmentProfitable(const Closure &C, RegDomain Domain) const;

  /// Calculate the total cost of reassigning the closure to \p Domain.
  double calculateCost(const Closure &C, RegDomain Domain) const;
};

char X86DomainReassignment::ID = 0;

} // End anonymous namespace.

bool X86DomainReassignment::visitRegister(Closure &C, Register Reg,
                                          RegDomain &Domain,
                                          SmallVectorImpl<Register> &Worklist) {
  if (!Reg.isVirtual())
    return true;

  auto I = EnclosedEdges.find(Reg);
  if (I != EnclosedEdges.end()) {
    if (I->second != C.getID()) {
      C.setAllIllegal();
      return false;
    }
    return true;
  }

  if (!MRI->hasOneDef(Reg))
    return true;

  RegDomain RD = getDomain(MRI->getRegClass(Reg), MRI->getTargetRegisterInfo());
  // First edge in closure sets the domain.
  if (Domain == NoDomain)
    Domain = RD;

  if (Domain != RD)
    return true;

  Worklist.push_back(Reg);
  return true;
}

bool X86DomainReassignment::encloseInstr(Closure &C, MachineInstr *MI) {
  auto [I, Inserted] = EnclosedInstrs.try_emplace(MI, C.getID());
  if (!Inserted) {
    if (I->second != C.getID()) {
      // Instruction already belongs to another closure, avoid conflicts between
      // closure and mark this closure as illegal.
      C.setAllIllegal();
      return false;
    }
    return true;
  }

  C.addInstruction(MI);

  // Mark closure as illegal for reassignment to domains, if there is no
  // converter for the instruction or if the converter cannot convert the
  // instruction.
  for (int i = 0; i != NumDomains; ++i) {
    if (C.isLegal((RegDomain)i)) {
      auto I = Converters.find({i, MI->getOpcode()});
      if (I == Converters.end() || !I->second->isLegal(MI, TII))
        C.setIllegal((RegDomain)i);
    }
  }
  return C.hasLegalDstDomain();
}

double X86DomainReassignment::calculateCost(const Closure &C,
                                            RegDomain DstDomain) const {
  assert(C.isLegal(DstDomain) && "Cannot calculate cost for illegal closure");

  double Cost = 0.0;
  for (auto *MI : C.instructions())
    Cost += Converters.find({DstDomain, MI->getOpcode()})
                ->second->getExtraCost(MI, MRI);
  return Cost;
}

bool X86DomainReassignment::isReassignmentProfitable(const Closure &C,
                                                     RegDomain Domain) const {
  return calculateCost(C, Domain) < 0.0;
}

void X86DomainReassignment::reassign(const Closure &C, RegDomain Domain) const {
  assert(C.isLegal(Domain) && "Cannot convert illegal closure");

  // Iterate all instructions in the closure, convert each one using the
  // appropriate converter.
  SmallVector<MachineInstr *, 8> ToErase;
  for (auto *MI : C.instructions())
    if (Converters.find({Domain, MI->getOpcode()})
            ->second->convertInstr(MI, TII, MRI))
      ToErase.push_back(MI);

  // Iterate all registers in the closure, replace them with registers in the
  // destination domain.
  for (Register Reg : C.edges()) {
    MRI->setRegClass(Reg, getDstRC(MRI->getRegClass(Reg), Domain));
    for (auto &MO : MRI->use_operands(Reg)) {
      if (MO.isReg())
        // Remove all subregister references as they are not valid in the
        // destination domain.
        MO.setSubReg(0);
    }
  }

  for (auto *MI : ToErase)
    MI->eraseFromParent();
}

/// \returns true when \p Reg is used as part of an address calculation in \p
/// MI.
static bool usedAsAddr(const MachineInstr &MI, Register Reg,
                       const TargetInstrInfo *TII) {
  if (!MI.mayLoadOrStore())
    return false;

  const MCInstrDesc &Desc = TII->get(MI.getOpcode());
  int MemOpStart = X86II::getMemoryOperandNo(Desc.TSFlags);
  if (MemOpStart == -1)
    return false;

  MemOpStart += X86II::getOperandBias(Desc);
  for (unsigned MemOpIdx = MemOpStart,
                MemOpEnd = MemOpStart + X86::AddrNumOperands;
       MemOpIdx < MemOpEnd; ++MemOpIdx) {
    const MachineOperand &Op = MI.getOperand(MemOpIdx);
    if (Op.isReg() && Op.getReg() == Reg)
      return true;
  }
  return false;
}

void X86DomainReassignment::buildClosure(Closure &C, Register Reg) {
  SmallVector<Register, 4> Worklist;
  RegDomain Domain = NoDomain;
  visitRegister(C, Reg, Domain, Worklist);
  while (!Worklist.empty()) {
    Register CurReg = Worklist.pop_back_val();

    // Register already in this closure.
    if (!C.insertEdge(CurReg))
      continue;
    EnclosedEdges[Reg] = C.getID();

    MachineInstr *DefMI = MRI->getVRegDef(CurReg);
    if (!encloseInstr(C, DefMI))
      return;

    // Add register used by the defining MI to the worklist.
    // Do not add registers which are used in address calculation, they will be
    // added to a different closure.
    int OpEnd = DefMI->getNumOperands();
    const MCInstrDesc &Desc = DefMI->getDesc();
    int MemOp = X86II::getMemoryOperandNo(Desc.TSFlags);
    if (MemOp != -1)
      MemOp += X86II::getOperandBias(Desc);
    for (int OpIdx = 0; OpIdx < OpEnd; ++OpIdx) {
      if (OpIdx == MemOp) {
        // skip address calculation.
        OpIdx += (X86::AddrNumOperands - 1);
        continue;
      }
      auto &Op = DefMI->getOperand(OpIdx);
      if (!Op.isReg() || !Op.isUse())
        continue;
      if (!visitRegister(C, Op.getReg(), Domain, Worklist))
        return;
    }

    // Expand closure through register uses.
    for (auto &UseMI : MRI->use_nodbg_instructions(CurReg)) {
      // We would like to avoid converting closures which calculare addresses,
      // as this should remain in GPRs.
      if (usedAsAddr(UseMI, CurReg, TII)) {
        C.setAllIllegal();
        return;
      }
      if (!encloseInstr(C, &UseMI))
        return;

      for (auto &DefOp : UseMI.defs()) {
        if (!DefOp.isReg())
          continue;

        Register DefReg = DefOp.getReg();
        if (!DefReg.isVirtual()) {
          C.setAllIllegal();
          return;
        }
        if (!visitRegister(C, DefReg, Domain, Worklist))
          return;
      }
    }
  }
}

void X86DomainReassignment::initConverters() {
  Converters[{MaskDomain, TargetOpcode::PHI}] =
      std::make_unique<InstrIgnore>(TargetOpcode::PHI);

  Converters[{MaskDomain, TargetOpcode::IMPLICIT_DEF}] =
      std::make_unique<InstrIgnore>(TargetOpcode::IMPLICIT_DEF);

  Converters[{MaskDomain, TargetOpcode::INSERT_SUBREG}] =
      std::make_unique<InstrReplaceWithCopy>(TargetOpcode::INSERT_SUBREG, 2);

  Converters[{MaskDomain, TargetOpcode::COPY}] =
      std::make_unique<InstrCOPYReplacer>(TargetOpcode::COPY, MaskDomain,
                                          TargetOpcode::COPY);

  auto createReplacerDstCOPY = [&](unsigned From, unsigned To) {
    Converters[{MaskDomain, From}] =
        std::make_unique<InstrReplacerDstCOPY>(From, To);
  };

#define GET_EGPR_IF_ENABLED(OPC) STI->hasEGPR() ? OPC##_EVEX : OPC
  createReplacerDstCOPY(X86::MOVZX32rm16, GET_EGPR_IF_ENABLED(X86::KMOVWkm));
  createReplacerDstCOPY(X86::MOVZX64rm16, GET_EGPR_IF_ENABLED(X86::KMOVWkm));

  createReplacerDstCOPY(X86::MOVZX32rr16, GET_EGPR_IF_ENABLED(X86::KMOVWkk));
  createReplacerDstCOPY(X86::MOVZX64rr16, GET_EGPR_IF_ENABLED(X86::KMOVWkk));

  if (STI->hasDQI()) {
    createReplacerDstCOPY(X86::MOVZX16rm8, GET_EGPR_IF_ENABLED(X86::KMOVBkm));
    createReplacerDstCOPY(X86::MOVZX32rm8, GET_EGPR_IF_ENABLED(X86::KMOVBkm));
    createReplacerDstCOPY(X86::MOVZX64rm8, GET_EGPR_IF_ENABLED(X86::KMOVBkm));

    createReplacerDstCOPY(X86::MOVZX16rr8, GET_EGPR_IF_ENABLED(X86::KMOVBkk));
    createReplacerDstCOPY(X86::MOVZX32rr8, GET_EGPR_IF_ENABLED(X86::KMOVBkk));
    createReplacerDstCOPY(X86::MOVZX64rr8, GET_EGPR_IF_ENABLED(X86::KMOVBkk));
  }

  auto createReplacer = [&](unsigned From, unsigned To) {
    Converters[{MaskDomain, From}] = std::make_unique<InstrReplacer>(From, To);
  };

  createReplacer(X86::MOV16rm, GET_EGPR_IF_ENABLED(X86::KMOVWkm));
  createReplacer(X86::MOV16mr, GET_EGPR_IF_ENABLED(X86::KMOVWmk));
  createReplacer(X86::MOV16rr, GET_EGPR_IF_ENABLED(X86::KMOVWkk));
  createReplacer(X86::SHR16ri, X86::KSHIFTRWki);
  createReplacer(X86::SHL16ri, X86::KSHIFTLWki);
  createReplacer(X86::NOT16r, X86::KNOTWkk);
  createReplacer(X86::OR16rr, X86::KORWkk);
  createReplacer(X86::AND16rr, X86::KANDWkk);
  createReplacer(X86::XOR16rr, X86::KXORWkk);

  bool HasNDD = STI->hasNDD();
  if (HasNDD) {
    createReplacer(X86::SHR16ri_ND, X86::KSHIFTRWki);
    createReplacer(X86::SHL16ri_ND, X86::KSHIFTLWki);
    createReplacer(X86::NOT16r_ND, X86::KNOTWkk);
    createReplacer(X86::OR16rr_ND, X86::KORWkk);
    createReplacer(X86::AND16rr_ND, X86::KANDWkk);
    createReplacer(X86::XOR16rr_ND, X86::KXORWkk);
  }

  if (STI->hasBWI()) {
    createReplacer(X86::MOV32rm, GET_EGPR_IF_ENABLED(X86::KMOVDkm));
    createReplacer(X86::MOV64rm, GET_EGPR_IF_ENABLED(X86::KMOVQkm));

    createReplacer(X86::MOV32mr, GET_EGPR_IF_ENABLED(X86::KMOVDmk));
    createReplacer(X86::MOV64mr, GET_EGPR_IF_ENABLED(X86::KMOVQmk));

    createReplacer(X86::MOV32rr, GET_EGPR_IF_ENABLED(X86::KMOVDkk));
    createReplacer(X86::MOV64rr, GET_EGPR_IF_ENABLED(X86::KMOVQkk));

    createReplacer(X86::SHR32ri, X86::KSHIFTRDki);
    createReplacer(X86::SHR64ri, X86::KSHIFTRQki);

    createReplacer(X86::SHL32ri, X86::KSHIFTLDki);
    createReplacer(X86::SHL64ri, X86::KSHIFTLQki);

    createReplacer(X86::ADD32rr, X86::KADDDkk);
    createReplacer(X86::ADD64rr, X86::KADDQkk);

    createReplacer(X86::NOT32r, X86::KNOTDkk);
    createReplacer(X86::NOT64r, X86::KNOTQkk);

    createReplacer(X86::OR32rr, X86::KORDkk);
    createReplacer(X86::OR64rr, X86::KORQkk);

    createReplacer(X86::AND32rr, X86::KANDDkk);
    createReplacer(X86::AND64rr, X86::KANDQkk);

    createReplacer(X86::ANDN32rr, X86::KANDNDkk);
    createReplacer(X86::ANDN64rr, X86::KANDNQkk);

    createReplacer(X86::XOR32rr, X86::KXORDkk);
    createReplacer(X86::XOR64rr, X86::KXORQkk);

    if (HasNDD) {
      createReplacer(X86::SHR32ri_ND, X86::KSHIFTRDki);
      createReplacer(X86::SHL32ri_ND, X86::KSHIFTLDki);
      createReplacer(X86::ADD32rr_ND, X86::KADDDkk);
      createReplacer(X86::NOT32r_ND, X86::KNOTDkk);
      createReplacer(X86::OR32rr_ND, X86::KORDkk);
      createReplacer(X86::AND32rr_ND, X86::KANDDkk);
      createReplacer(X86::XOR32rr_ND, X86::KXORDkk);
      createReplacer(X86::SHR64ri_ND, X86::KSHIFTRQki);
      createReplacer(X86::SHL64ri_ND, X86::KSHIFTLQki);
      createReplacer(X86::ADD64rr_ND, X86::KADDQkk);
      createReplacer(X86::NOT64r_ND, X86::KNOTQkk);
      createReplacer(X86::OR64rr_ND, X86::KORQkk);
      createReplacer(X86::AND64rr_ND, X86::KANDQkk);
      createReplacer(X86::XOR64rr_ND, X86::KXORQkk);
    }

    // TODO: KTEST is not a replacement for TEST due to flag differences. Need
    // to prove only Z flag is used.
    // createReplacer(X86::TEST32rr, X86::KTESTDkk);
    // createReplacer(X86::TEST64rr, X86::KTESTQkk);
  }

  if (STI->hasDQI()) {
    createReplacer(X86::ADD8rr, X86::KADDBkk);
    createReplacer(X86::ADD16rr, X86::KADDWkk);

    createReplacer(X86::AND8rr, X86::KANDBkk);

    createReplacer(X86::MOV8rm, GET_EGPR_IF_ENABLED(X86::KMOVBkm));
    createReplacer(X86::MOV8mr, GET_EGPR_IF_ENABLED(X86::KMOVBmk));
    createReplacer(X86::MOV8rr, GET_EGPR_IF_ENABLED(X86::KMOVBkk));

    createReplacer(X86::NOT8r, X86::KNOTBkk);

    createReplacer(X86::OR8rr, X86::KORBkk);

    createReplacer(X86::SHR8ri, X86::KSHIFTRBki);
    createReplacer(X86::SHL8ri, X86::KSHIFTLBki);

    // TODO: KTEST is not a replacement for TEST due to flag differences. Need
    // to prove only Z flag is used.
    // createReplacer(X86::TEST8rr, X86::KTESTBkk);
    // createReplacer(X86::TEST16rr, X86::KTESTWkk);

    createReplacer(X86::XOR8rr, X86::KXORBkk);

    if (HasNDD) {
      createReplacer(X86::ADD8rr_ND, X86::KADDBkk);
      createReplacer(X86::ADD16rr_ND, X86::KADDWkk);
      createReplacer(X86::AND8rr_ND, X86::KANDBkk);
      createReplacer(X86::NOT8r_ND, X86::KNOTBkk);
      createReplacer(X86::OR8rr_ND, X86::KORBkk);
      createReplacer(X86::SHR8ri_ND, X86::KSHIFTRBki);
      createReplacer(X86::SHL8ri_ND, X86::KSHIFTLBki);
      createReplacer(X86::XOR8rr_ND, X86::KXORBkk);
    }
  }
#undef GET_EGPR_IF_ENABLED
}

bool X86DomainReassignment::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  if (DisableX86DomainReassignment)
    return false;

  LLVM_DEBUG(
      dbgs() << "***** Machine Function before Domain Reassignment *****\n");
  LLVM_DEBUG(MF.print(dbgs()));

  STI = &MF.getSubtarget<X86Subtarget>();
  // GPR->K is the only transformation currently supported, bail out early if no
  // AVX512.
  // TODO: We're also bailing of AVX512BW isn't supported since we use VK32 and
  // VK64 for GR32/GR64, but those aren't legal classes on KNL. If the register
  // coalescer doesn't clean it up and we generate a spill we will crash.
  if (!STI->hasAVX512() || !STI->hasBWI())
    return false;

  MRI = &MF.getRegInfo();
  assert(MRI->isSSA() && "Expected MIR to be in SSA form");

  TII = STI->getInstrInfo();
  initConverters();
  bool Changed = false;

  EnclosedEdges.clear();
  EnclosedInstrs.clear();

  std::vector<Closure> Closures;

  // Go over all virtual registers and calculate a closure.
  unsigned ClosureID = 0;
  for (unsigned Idx = 0; Idx < MRI->getNumVirtRegs(); ++Idx) {
    Register Reg = Register::index2VirtReg(Idx);

    // Skip unused VRegs.
    if (MRI->reg_nodbg_empty(Reg))
      continue;

    // GPR only current source domain supported.
    if (!MRI->getTargetRegisterInfo()->isGeneralPurposeRegisterClass(
            MRI->getRegClass(Reg)))
      continue;

    // Register already in closure.
    if (EnclosedEdges.contains(Reg))
      continue;

    // Calculate closure starting with Reg.
    Closure C(ClosureID++, {MaskDomain});
    buildClosure(C, Reg);

    // Collect all closures that can potentially be converted.
    if (!C.empty() && C.isLegal(MaskDomain))
      Closures.push_back(std::move(C));
  }

  for (Closure &C : Closures) {
    LLVM_DEBUG(C.dump(MRI));
    if (isReassignmentProfitable(C, MaskDomain)) {
      reassign(C, MaskDomain);
      ++NumClosuresConverted;
      Changed = true;
    }
  }

  LLVM_DEBUG(
      dbgs() << "***** Machine Function after Domain Reassignment *****\n");
  LLVM_DEBUG(MF.print(dbgs()));

  return Changed;
}

INITIALIZE_PASS(X86DomainReassignment, "x86-domain-reassignment",
                "X86 Domain Reassignment Pass", false, false)

/// Returns an instance of the Domain Reassignment pass.
FunctionPass *llvm::createX86DomainReassignmentPass() {
  return new X86DomainReassignment();
}

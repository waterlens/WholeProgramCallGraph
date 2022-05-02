//===- CallGraph.cpp - Build a Module's call graph ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CallGraph.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <fmt/core.h>
#include <llvm/Support/Casting.h>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Implementations of the CallGraph class methods.
//

CallGraph::CallGraph(Module &M)
    : M(M), ExternalCallingNode(getOrInsertFunction(nullptr)),
      CallsExternalNode(std::make_unique<CallGraphNode>(this, nullptr)) {
  // Add every interesting function to the call graph.
  for (Function &F : M)
    if (!isDbgInfoIntrinsic(F.getIntrinsicID()))
      addToCallGraph(&F);
}

CallGraph::CallGraph(CallGraph &&Arg)
    : M(Arg.M), FunctionMap(std::move(Arg.FunctionMap)),
      ExternalCallingNode(Arg.ExternalCallingNode),
      CallsExternalNode(std::move(Arg.CallsExternalNode)) {
  Arg.FunctionMap.clear();
  Arg.ExternalCallingNode = nullptr;

  // Update parent CG for all call graph's nodes.
  CallsExternalNode->CG = this;
  for (auto &P : FunctionMap)
    P.second->CG = this;
}

CallGraph::~CallGraph() {
  // CallsExternalNode is not in the function map, delete it explicitly.
  if (CallsExternalNode)
    CallsExternalNode->allReferencesDropped();

// Reset all node's use counts to zero before deleting them to prevent an
// assertion from firing.
#ifndef NDEBUG
  for (auto &I : FunctionMap)
    I.second->allReferencesDropped();
#endif
}

void CallGraph::addToCallGraph(Function *F) {
  CallGraphNode *Node = getOrInsertFunction(F);

  // If this function has external linkage or has its address taken and
  // it is not a callback, then anything could call it.
  if (!F->hasLocalLinkage() ||
      F->hasAddressTaken(nullptr, /*IgnoreCallbackUses=*/true,
                         /* IgnoreAssumeLikeCalls */ true,
                         /* IgnoreLLVMUsed */ false))
    ExternalCallingNode->addCalledFunction(nullptr, Node);

  populateCallGraphNode(Node);
}

void CallGraph::populateCallGraphNode(CallGraphNode *Node) {
  Function *F = Node->getFunction();

  // If this function is not defined in this translation unit, it could call
  // anything.
  if (F->isDeclaration() && !F->isIntrinsic())
    Node->addCalledFunction(nullptr, CallsExternalNode.get());

  // Look for calls by this function.
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        const Function *Callee = Call->getCalledFunction();
        if (!Callee || !Intrinsic::isLeaf(Callee->getIntrinsicID()))
          // Indirect calls of intrinsics are not allowed so no need to check.
          // We can be more precise here by using TargetArg returned by
          // Intrinsic::isLeaf.
          Node->addCalledFunction(Call, CallsExternalNode.get());
        else if (!Callee->isIntrinsic())
          Node->addCalledFunction(Call, getOrInsertFunction(Callee));

        // Add reference to callback functions.
        forEachCallbackFunction(*Call, [=](Function *CB) {
          Node->addCalledFunction(nullptr, getOrInsertFunction(CB));
        });
      }
    }
}

void CallGraph::print() const {
  // Print in a deterministic order by sorting CallGraphNodes by name.  We do
  // this here to avoid slowing down the non-printing fast path.

  SmallVector<CallGraphNode *, 16> Nodes;
  Nodes.reserve(FunctionMap.size());

  for (const auto &I : *this)
    Nodes.push_back(I.second.get());

  llvm::sort(Nodes, [](CallGraphNode *LHS, CallGraphNode *RHS) {
    if (Function *LF = LHS->getFunction())
      if (Function *RF = RHS->getFunction())
        return LF->getName() < RF->getName();

    return RHS->getFunction() != nullptr;
  });

  for (CallGraphNode *CN : Nodes)
    CN->print();
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void CallGraph::dump() const { print(); }
#endif

void CallGraph::ReplaceExternalCallEdge(CallGraphNode *Old,
                                        CallGraphNode *New) {
  for (auto &CR : ExternalCallingNode->CalledFunctions)
    if (CR.second == Old) {
      CR.second->DropRef();
      CR.second = New;
      CR.second->AddRef();
    }
}

// removeFunctionFromModule - Unlink the function from this module, returning
// it.  Because this removes the function from the module, the call graph node
// is destroyed.  This is only valid if the function does not call any other
// functions (ie, there are no edges in it's CGN).  The easiest way to do this
// is to dropAllReferences before calling this.
//
Function *CallGraph::removeFunctionFromModule(CallGraphNode *CGN) {
  assert(CGN->empty() && "Cannot remove function from call "
                         "graph if it references other functions!");
  Function *F = CGN->getFunction(); // Get the function for the call graph node
  FunctionMap.erase(F);             // Remove the call graph node from the map

  M.getFunctionList().remove(F);
  return F;
}

// getOrInsertFunction - This method is identical to calling operator[], but
// it will insert a new CallGraphNode for the specified function if one does
// not already exist.
CallGraphNode *CallGraph::getOrInsertFunction(const Function *F) {
  auto &CGN = FunctionMap[F];
  if (CGN)
    return CGN.get();

  assert((!F || F->getParent() == &M) && "Function not in current module!");
  CGN = std::make_unique<CallGraphNode>(this, const_cast<Function *>(F));
  return CGN.get();
}

//===----------------------------------------------------------------------===//
// Implementations of the CallGraphNode class methods.
//

void CallGraphNode::print() const {
  if (Function *F = getFunction())
    fmt::print("function: '{}'", F->getName().str());
  else
    fmt::print("function: <<null>>");

  fmt::print(" # uses={}\n", getNumReferences());

  for (const auto &I : *this) {
    fmt::print("    ");
    if (I.first.hasValue()) {
      Value *value = I.first.getValue();
      if (auto *Call = dyn_cast<CallBase>(value)) {
        fmt::print("from '{}' ", Call->getParent()->getName().str());
      }
    }
    if (Function *FI = I.second->getFunction())
      fmt::print("calls function '{}'\n", FI->getName().str());
    else
      fmt::print("external function\n");
  }
  fmt::print("\n");
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void CallGraphNode::dump() const { print(); }
#endif

/// removeCallEdgeFor - This method removes the edge in the node for the
/// specified call site.  Note that this method takes linear time, so it
/// should be used sparingly.
void CallGraphNode::removeCallEdgeFor(CallBase &Call) {
  for (CalledFunctionsVector::iterator I = CalledFunctions.begin();; ++I) {
    assert(I != CalledFunctions.end() && "Cannot find callsite to remove!");
    if (I->first && *I->first == &Call) {
      I->second->DropRef();
      *I = CalledFunctions.back();
      CalledFunctions.pop_back();

      // Remove all references to callback functions if there are any.
      forEachCallbackFunction(Call, [=](Function *CB) {
        removeOneAbstractEdgeTo(CG->getOrInsertFunction(CB));
      });
      return;
    }
  }
}

// removeAnyCallEdgeTo - This method removes any call edges from this node to
// the specified callee function.  This takes more time to execute than
// removeCallEdgeTo, so it should not be used unless necessary.
void CallGraphNode::removeAnyCallEdgeTo(CallGraphNode *Callee) {
  for (unsigned i = 0, e = CalledFunctions.size(); i != e; ++i)
    if (CalledFunctions[i].second == Callee) {
      Callee->DropRef();
      CalledFunctions[i] = CalledFunctions.back();
      CalledFunctions.pop_back();
      --i;
      --e;
    }
}

/// removeOneAbstractEdgeTo - Remove one edge associated with a null callsite
/// from this node to the specified callee function.
void CallGraphNode::removeOneAbstractEdgeTo(CallGraphNode *Callee) {
  for (CalledFunctionsVector::iterator I = CalledFunctions.begin();; ++I) {
    assert(I != CalledFunctions.end() && "Cannot find callee to remove!");
    CallRecord &CR = *I;
    if (CR.second == Callee && !CR.first) {
      Callee->DropRef();
      *I = CalledFunctions.back();
      CalledFunctions.pop_back();
      return;
    }
  }
}

/// replaceCallEdge - This method replaces the edge in the node for the
/// specified call site with a new one.  Note that this method takes linear
/// time, so it should be used sparingly.
void CallGraphNode::replaceCallEdge(CallBase &Call, CallBase &NewCall,
                                    CallGraphNode *NewNode) {
  for (CalledFunctionsVector::iterator I = CalledFunctions.begin();; ++I) {
    assert(I != CalledFunctions.end() && "Cannot find callsite to remove!");
    if (I->first && *I->first == &Call) {
      I->second->DropRef();
      I->first = &NewCall;
      I->second = NewNode;
      NewNode->AddRef();

      // Refresh callback references. Do not resize CalledFunctions if the
      // number of callbacks is the same for new and old call sites.
      SmallVector<CallGraphNode *, 4u> OldCBs;
      SmallVector<CallGraphNode *, 4u> NewCBs;
      forEachCallbackFunction(Call, [this, &OldCBs](Function *CB) {
        OldCBs.push_back(CG->getOrInsertFunction(CB));
      });
      forEachCallbackFunction(NewCall, [this, &NewCBs](Function *CB) {
        NewCBs.push_back(CG->getOrInsertFunction(CB));
      });
      if (OldCBs.size() == NewCBs.size()) {
        for (unsigned N = 0; N < OldCBs.size(); ++N) {
          CallGraphNode *OldNode = OldCBs[N];
          CallGraphNode *NewNode = NewCBs[N];
          for (auto J = CalledFunctions.begin();; ++J) {
            assert(J != CalledFunctions.end() &&
                   "Cannot find callsite to update!");
            if (!J->first && J->second == OldNode) {
              J->second = NewNode;
              OldNode->DropRef();
              NewNode->AddRef();
              break;
            }
          }
        }
      } else {
        for (auto *CGN : OldCBs)
          removeOneAbstractEdgeTo(CGN);
        for (auto *CGN : NewCBs)
          addCalledFunction(nullptr, CGN);
      }
      return;
    }
  }
}
